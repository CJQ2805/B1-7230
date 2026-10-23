/*
 * 文件作用：
 *   Flash均衡擦写中间件，负责在指定Flash区域内按槽位循环保存定长记录。
 *
 * 主要用途：
 *   1. 给LOG模块提供掉电可续写的记录存储。
 *   2. 通过页轮换和槽位序号减少单页反复擦写。
 *   3. 初始化时扫描已有记录，找到下一次写入位置。
 *
 * 适用范围：
 *   与具体业务无关，但依赖底层 d_flash 抽象接口。迁移到其他项目时需要提供同名Flash接口。
 *
 * 调用关系：
 *   app_log_manage 调用本模块；本模块调用 d_flash 和 d_watchdog。
 *
 * 注意事项：
 *   记录长度、页大小和地址范围必须由调用方配置正确；Flash写入期间可能阻塞主循环。
 */
#include "Middle/m_flash_wl.h"/* 本模块对外类型和接口声明。 */

#include "Driver/d_flash.h" /* 所有Flash硬件操作统一通过Driver接口完成。 */
#include "Driver/d_watchdog.h" /* 长时间格式化期间继续主动喂看门狗。 */
#include <stddef.h>           /* 使用offsetof()计算结构体字段偏移。 */
#include <string.h>           /* 使用memset()初始化页头和记录头。 */

/*
 * 均衡擦写整体流程：
 * 1. 一个分区由多个Flash页组成，每页开头写一个页头。
 * 2. 每条记录固定占用record_size字节，内部包含记录头、业务数据和提交标志。
 * 3. 写记录时先写记录头，再写业务数据，最后写提交标志。
 * 4. 掉电时如果没有写到提交标志，这条记录启动扫描时会被当成无效记录。
 * 5. 当前页写满后擦除下一页并写新页头，形成循环使用，从而实现均衡擦写。
 */

#define M_FLASH_WL_PAGE_MAGIC          ((uint32_t)0x44574C50U)  /* 页头魔术字，ASCII含义近似DWLP，用于判断页头是否有效。 */
#define M_FLASH_WL_RECORD_MAGIC        ((uint32_t)0x44574C52U)  /* 记录头魔术字，ASCII含义近似DWLR，用于判断记录头是否有效。 */
#define M_FLASH_WL_COMMIT_MAGIC        ((uint32_t)0x44574C43U)  /* 提交魔术字，ASCII含义近似DWLC，最后写入表示记录完整。 */
#define M_FLASH_WL_INVALID_ADDR        ((uint32_t)0xFFFFFFFFU)  /* 无效地址标记，用于latest_addr/write_addr初始化。 */
#define M_FLASH_WL_VERSION             ((uint8_t)1U)            /* 页头格式版本，后续结构变化时用于区分。 */

/* 每个Flash页开头保存这个页头，用于识别页属于哪个LOG分区、页序号是多少。 */
typedef struct
{
    uint32_t magic;            /* 页头魔术字，必须等于M_FLASH_WL_PAGE_MAGIC。 */
    uint32_t page_seq;         /* 页序号，每切换到新页加1，用于启动时找到最新页。 */
    uint32_t erase_count;      /* 当前页累计擦除次数，用于后续评估磨损。 */
    uint32_t area_start;       /* 分区起始地址，用于确认页头和当前配置匹配。 */
    uint32_t area_size;        /* 分区总大小，用于确认页头和当前配置匹配。 */
    uint16_t page_size;        /* Flash页大小，当前固定为2K。 */
    uint16_t page_count;       /* 分区页数量。 */
    uint16_t record_size;      /* 每条记录固定占用的Flash槽位大小。 */
    uint16_t records_per_page; /* 每页最多能放多少条记录。 */
    uint8_t area_id;           /* 分区类型，由上层传入，例如实时故障LOG。 */
    uint8_t version;           /* 页头格式版本。 */
    uint16_t header_crc;       /* 页头CRC，只计算header_crc之前的字段。 */
    uint32_t reserved[8];      /* 预留字段，保持页头固定64字节。 */
} m_flash_wl_page_header_t;

/* 每条记录开头保存这个记录头，用于识别记录序号、业务数据长度和CRC。 */
typedef struct
{
    uint32_t magic;       /* 记录头魔术字，必须等于M_FLASH_WL_RECORD_MAGIC。 */
    uint32_t record_seq;  /* 记录序号，每成功写一条加1，用于找到最新记录。 */
    uint16_t payload_len; /* 业务数据真实长度，可以小于槽位可用payload_size。 */
    uint16_t payload_crc; /* 业务数据CRC，用于判断数据是否完整。 */
    uint16_t record_size; /* 当前记录槽位大小，用于确认记录和配置匹配。 */
    uint16_t header_crc;  /* 记录头CRC，只计算header_crc之前的字段。 */
} m_flash_wl_record_header_t;

/* 扫描单个Flash页后的临时结果。 */
typedef struct
{
    bool valid;              /* 页头是否有效。 */
    uint32_t page_seq;       /* 页头中的页序号。 */
    uint32_t first_free_addr;/* 本页第一个空白记录槽地址。 */
    uint16_t valid_count;    /* 本页扫描到的有效记录数量。 */
} m_flash_wl_page_scan_t;

/* 计算CRC16-CCITT，用于页头、记录头和业务数据校验。 */
static uint16_t m_flash_wl_crc16(const uint8_t *data, uint32_t len)
{
    uint32_t u32ByteIndex;              /* 字节循环下标。 */
    uint8_t u8BitIndex;                 /* 单个字节内部的bit循环下标。 */
    uint16_t u16Crc16 = 0xFFFFU;        /* CRC初值。 */

    for(u32ByteIndex = 0U; u32ByteIndex < len; u32ByteIndex++) /* 逐字节处理输入数据。 */
    {
        u16Crc16 ^= (uint16_t)data[u32ByteIndex] << 8; /* 当前字节放到CRC高8位参与计算。 */
        for(u8BitIndex = 0U; u8BitIndex < 8U; u8BitIndex++)   /* 每个字节处理8个bit。 */
        {
            if((u16Crc16 & 0x8000U) != 0U)  /* 如果当前CRC最高位为1。 */
            {
                u16Crc16 = (uint16_t)((u16Crc16 << 1) ^ 0x1021U); /* 左移后异或多项式。 */
            }
            else                       /* 如果当前CRC最高位为0。 */
            {
                u16Crc16 <<= 1;             /* 只左移，不异或多项式。 */
            }
        }
    }

    return u16Crc16; /* 返回最终CRC值。 */
}

/* 判断地址或长度是否按4字节对齐，N32G45X按word写Flash。 */
static bool m_flash_wl_is_word_aligned(uint32_t value)
{
    return ((value & 0x03U) == 0U); /* 低2位为0表示4字节对齐。 */
}

/* 判断地址是否按Flash页大小对齐。 */
static bool m_flash_wl_is_page_aligned(uint32_t value)
{
    return ((value % M_FLASH_WL_PAGE_SIZE) == 0U); /* 地址能整除页大小表示页对齐。 */
}

/* 计算单页最多可以放多少条固定长度记录。 */
static uint16_t m_flash_wl_calc_records_per_page(uint16_t record_size)
{
    return (uint16_t)((M_FLASH_WL_PAGE_SIZE - M_FLASH_WL_PAGE_HEADER_SIZE) / record_size); /* 页总空间减去页头后再除以记录槽位。 */
}

/* 根据页数量计算整个均衡擦写分区占用多少字节。 */
static uint32_t m_flash_wl_area_size(const m_flash_wl_cfg_t *cfg)
{
    return (uint32_t)cfg->page_count * M_FLASH_WL_PAGE_SIZE; /* 分区大小 = 页数量 * 每页大小。 */
}

/* 根据页序号计算对应Flash页的起始地址。 */
static uint32_t m_flash_wl_page_addr(const m_flash_wl_cfg_t *cfg, uint16_t page_index)
{
    return cfg->start_addr + ((uint32_t)page_index * M_FLASH_WL_PAGE_SIZE); /* 分区起始地址加页偏移。 */
}

/* 根据页序号计算对应Flash页的结束地址。 */
static uint32_t m_flash_wl_page_end_addr(const m_flash_wl_cfg_t *cfg, uint16_t page_index)
{
    return m_flash_wl_page_addr(cfg, page_index) + M_FLASH_WL_PAGE_SIZE; /* 页起始地址加页大小。 */
}

/* 判断指定Flash范围是否全是擦除态。 */
static bool m_flash_wl_is_blank(uint32_t addr, uint32_t len)
{
    return d_flash_is_blank(addr, len); /* 空白检查由Driver统一处理范围和硬件读取。 */
}

/* 判断整个均衡擦写分区是否完全空白。 */
static bool m_flash_wl_is_area_blank(const m_flash_wl_cfg_t *cfg)
{
    return m_flash_wl_is_blank(cfg->start_addr, m_flash_wl_area_size(cfg)); /* 从分区起始地址检查整个分区大小。 */
}

/* 检查上层传入的分区配置是否合法。 */
static m_flash_wl_status_t m_flash_wl_validate_cfg(const m_flash_wl_cfg_t *cfg)
{
    uint32_t u32AreaEndAddress; /* 分区结束地址。 */

    if(cfg == NULL) /* 配置指针不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    if((cfg->page_count < 2U) || (cfg->record_size < M_FLASH_WL_MIN_RECORD_SIZE)) /* 至少2页才能轮换，记录槽位也必须放得下头、数据和提交标志。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    if((cfg->record_size % 4U) != 0U) /* 记录槽位必须4字节对齐，方便按word写Flash。 */
    {
        return M_FLASH_WL_ERR_ALIGN; /* 返回对齐错误。 */
    }

    if(m_flash_wl_calc_records_per_page(cfg->record_size) == 0U) /* 如果一页去掉页头后连一条记录都放不下。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    if(!m_flash_wl_is_page_aligned(cfg->start_addr)) /* 分区起始地址必须按Flash页对齐。 */
    {
        return M_FLASH_WL_ERR_ALIGN; /* 返回对齐错误。 */
    }

    u32AreaEndAddress = cfg->start_addr + m_flash_wl_area_size(cfg); /* 计算分区结束地址。 */
    if((cfg->start_addr < M_FLASH_WL_FLASH_START_ADDR) || /* 起始地址不能低于Flash起始地址。 */
       (u32AreaEndAddress > M_FLASH_WL_FLASH_END_ADDR) ||          /* 结束地址不能超过Flash结束地址。 */
       (u32AreaEndAddress <= cfg->start_addr))                     /* 防止地址计算溢出。 */
    {
        return M_FLASH_WL_ERR_RANGE; /* 返回范围错误。 */
    }

    return M_FLASH_WL_OK; /* 配置检查通过。 */
}

/* 向Flash写入一个32位word。 */
static m_flash_wl_status_t m_flash_wl_program_word(uint32_t addr, uint32_t data)
{
    if(!m_flash_wl_is_word_aligned(addr)) /* 写地址必须4字节对齐。 */
    {
        return M_FLASH_WL_ERR_ALIGN; /* 返回对齐错误。 */
    }

    if(!d_flash_check_range(addr, sizeof(data))) /* 写入范围必须落在Flash内且不能溢出。 */
    {
        return M_FLASH_WL_ERR_RANGE; /* 返回范围错误。 */
    }

    if(!d_flash_write(addr, (const uint8_t *)&data, sizeof(data))) /* Driver负责空白检查、编程和回读。 */
    {
        return M_FLASH_WL_ERR_FLASH; /* 底层写入失败统一映射为Flash错误。 */
    }

    return M_FLASH_WL_OK; /* word写入成功。 */
}

/* 把任意长度的数据按4字节补齐后写入Flash。 */
static m_flash_wl_status_t m_flash_wl_program_buffer(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if(!m_flash_wl_is_word_aligned(addr)) /* 起始地址必须4字节对齐。 */
    {
        return M_FLASH_WL_ERR_ALIGN; /* 返回对齐错误。 */
    }

    if(len == 0U) /* 沿用原循环在零长度时不执行写入并返回成功的行为。 */
    {
        return M_FLASH_WL_OK;
    }

    if(!d_flash_check_range(addr, len)) /* 写入范围必须落在Flash内且不能溢出。 */
    {
        return M_FLASH_WL_ERR_RANGE;
    }

    if(!d_flash_write(addr, data, len)) /* Driver按word补齐并完成逐字回读。 */
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    return M_FLASH_WL_OK; /* 整个buffer写入成功。 */
}

/* 擦除一个Flash页，并验证擦除后是否全为0xFFFFFFFF。 */
static m_flash_wl_status_t m_flash_wl_erase_page(uint32_t page_addr)
{
    if(!d_flash_is_page_aligned(page_addr)) /* 页地址错误时保留明确的对齐错误。 */
    {
        return M_FLASH_WL_ERR_ALIGN;
    }

    if(!d_flash_check_range(page_addr, M_FLASH_WL_PAGE_SIZE)) /* 擦除范围必须完整落在Flash内。 */
    {
        return M_FLASH_WL_ERR_RANGE;
    }

    if(!d_flash_erase_page(page_addr)) /* Driver负责准备、擦除和锁回Flash。 */
    {
        return M_FLASH_WL_ERR_FLASH; /* 返回Flash错误。 */
    }

    if(!m_flash_wl_is_blank(page_addr, M_FLASH_WL_PAGE_SIZE)) /* 擦除后再检查整页是否空白。 */
    {
        return M_FLASH_WL_ERR_FLASH; /* 不是空白，说明擦除失败。 */
    }

    return M_FLASH_WL_OK; /* 擦除成功。 */
}

/* 检查指定页的页头是否有效，并可选拷贝页头内容。 */
static bool m_flash_wl_page_header_valid(const m_flash_wl_cfg_t *cfg, uint16_t page_index,
                                         m_flash_wl_page_header_t *out_header)
{
    const m_flash_wl_page_header_t *ptPageHeader; /* 指向Flash中的页头。 */
    uint16_t u16HeaderCrc;                  /* 临时保存计算出的页头CRC。 */

    ptPageHeader = (const m_flash_wl_page_header_t *)m_flash_wl_page_addr(cfg, page_index); /* 把当前页起始地址当作页头。 */

    if(ptPageHeader->magic != M_FLASH_WL_PAGE_MAGIC) /* 魔术字不对，说明这页不是有效均衡擦写页。 */
    {
        return false; /* 页头无效。 */
    }

    if((ptPageHeader->version != M_FLASH_WL_VERSION) ||                                  /* 页头版本必须匹配。 */
       (ptPageHeader->area_id != cfg->area_id) ||                                        /* 分区ID必须匹配。 */
       (ptPageHeader->area_start != cfg->start_addr) ||                                  /* 分区起始地址必须匹配。 */
       (ptPageHeader->area_size != m_flash_wl_area_size(cfg)) ||                         /* 分区大小必须匹配。 */
       (ptPageHeader->page_size != M_FLASH_WL_PAGE_SIZE) ||                              /* 页大小必须匹配。 */
       (ptPageHeader->page_count != cfg->page_count) ||                                  /* 页数量必须匹配。 */
       (ptPageHeader->record_size != cfg->record_size) ||                                /* 记录槽位大小必须匹配。 */
       (ptPageHeader->records_per_page != m_flash_wl_calc_records_per_page(cfg->record_size))) /* 每页记录数必须匹配。 */
    {
        return false; /* 任意配置不匹配，都认为页头无效。 */
    }

    u16HeaderCrc = m_flash_wl_crc16((const uint8_t *)ptPageHeader, offsetof(m_flash_wl_page_header_t, header_crc)); /* 只计算header_crc之前的字段。 */
    if(u16HeaderCrc != ptPageHeader->header_crc) /* CRC不一致说明页头被破坏或没写完整。 */
    {
        return false; /* 页头无效。 */
    }

    if(out_header != NULL) /* 如果调用者需要页头内容。 */
    {
        *out_header = *ptPageHeader; /* 拷贝页头到输出参数。 */
    }

    return true; /* 页头有效。 */
}

/* 在指定页写入页头。 */
static m_flash_wl_status_t m_flash_wl_write_page_header(const m_flash_wl_cfg_t *cfg, uint16_t page_index,
                                                        uint32_t page_seq, uint32_t erase_count)
{
    m_flash_wl_page_header_t tPageHeader; /* RAM中的页头结构体。 */
    uint32_t u32PageAddress;         /* 目标页起始地址。 */

    memset(&tPageHeader, 0xFF, sizeof(tPageHeader)); /* 先填0xFF，保持预留字段为Flash擦除态。 */
    tPageHeader.magic = M_FLASH_WL_PAGE_MAGIC;  /* 写入页头魔术字。 */
    tPageHeader.page_seq = page_seq;            /* 写入页序号。 */
    tPageHeader.erase_count = erase_count;      /* 写入擦除次数。 */
    tPageHeader.area_start = cfg->start_addr;   /* 写入分区起始地址。 */
    tPageHeader.area_size = m_flash_wl_area_size(cfg); /* 写入分区大小。 */
    tPageHeader.page_size = (uint16_t)M_FLASH_WL_PAGE_SIZE; /* 写入页大小。 */
    tPageHeader.page_count = cfg->page_count;   /* 写入分区页数量。 */
    tPageHeader.record_size = cfg->record_size; /* 写入记录槽位大小。 */
    tPageHeader.records_per_page = m_flash_wl_calc_records_per_page(cfg->record_size); /* 写入每页记录数。 */
    tPageHeader.area_id = cfg->area_id;         /* 写入分区ID。 */
    tPageHeader.version = M_FLASH_WL_VERSION;   /* 写入页头版本。 */
    tPageHeader.header_crc = m_flash_wl_crc16((const uint8_t *)&tPageHeader,
                                          offsetof(m_flash_wl_page_header_t, header_crc)); /* 写入页头CRC。 */

    u32PageAddress = m_flash_wl_page_addr(cfg, page_index); /* 计算目标页起始地址。 */
    return m_flash_wl_program_buffer(u32PageAddress, (const uint8_t *)&tPageHeader, sizeof(tPageHeader)); /* 把页头写入Flash。 */
}

/* 检查指定地址处的一条记录是否有效。 */
static bool m_flash_wl_record_valid(const m_flash_wl_area_t *area, uint32_t addr,
                                    uint32_t *out_seq, uint16_t *out_len)
{
    const m_flash_wl_record_header_t *ptRecordHeader; /* 指向Flash中的记录头。 */
    uint32_t u32CommitAddress;                       /* 记录槽位末尾的提交标志地址。 */
    uint16_t u16HeaderCrc;                            /* 临时保存记录头CRC。 */
    uint16_t u16PayloadCrc;                           /* 临时保存业务数据CRC。 */

    ptRecordHeader = (const m_flash_wl_record_header_t *)addr; /* 把记录槽位起始地址当作记录头。 */
    u32CommitAddress = addr + area->cfg.record_size - M_FLASH_WL_RECORD_COMMIT_SIZE; /* 提交标志在槽位最后4字节。 */

    if(*((volatile const uint32_t *)u32CommitAddress) != M_FLASH_WL_COMMIT_MAGIC) /* 先看提交标志是否写完。 */
    {
        return false; /* 没有提交标志，说明记录未完成或掉电中断。 */
    }

    if((ptRecordHeader->magic != M_FLASH_WL_RECORD_MAGIC) ||        /* 记录魔术字必须正确。 */
       (ptRecordHeader->record_size != area->cfg.record_size) ||    /* 记录槽位大小必须匹配当前配置。 */
       (ptRecordHeader->payload_len > area->payload_size))          /* 业务数据长度不能超过槽位可用长度。 */
    {
        return false; /* 记录头基础字段无效。 */
    }

    u16HeaderCrc = m_flash_wl_crc16((const uint8_t *)ptRecordHeader, offsetof(m_flash_wl_record_header_t, header_crc)); /* 计算记录头CRC。 */
    if(u16HeaderCrc != ptRecordHeader->header_crc) /* 记录头CRC不一致。 */
    {
        return false; /* 记录无效。 */
    }

    u16PayloadCrc = m_flash_wl_crc16((const uint8_t *)(addr + M_FLASH_WL_RECORD_HEADER_SIZE),
                                     ptRecordHeader->payload_len); /* 计算业务数据CRC。 */
    if(u16PayloadCrc != ptRecordHeader->payload_crc) /* 业务数据CRC不一致。 */
    {
        return false; /* 记录无效。 */
    }

    if(out_seq != NULL) /* 如果调用者需要记录序号。 */
    {
        *out_seq = ptRecordHeader->record_seq; /* 输出记录序号。 */
    }

    if(out_len != NULL) /* 如果调用者需要业务数据长度。 */
    {
        *out_len = ptRecordHeader->payload_len; /* 输出业务数据长度。 */
    }

    return true; /* 记录完整且校验通过。 */
}

/* 统计指定页内当前仍然有效的记录数量，切页覆盖旧页前用于修正总条数。 */
static uint16_t m_flash_wl_count_valid_records_on_page(const m_flash_wl_area_t *area, uint16_t page_index)
{
    uint32_t u32RecordAddress;       /* 当前扫描槽位地址。 */
    uint32_t u32PageEndAddress;      /* 当前页结束地址。 */
    uint16_t u16ValidRecordCount = 0U; /* 当前页有效记录数量。 */
    uint16_t u16SlotIndex;           /* 槽位循环下标。 */

    if(!m_flash_wl_page_header_valid(&area->cfg, page_index, NULL)) /* 页头无效则不统计。 */
    {
        return 0U; /* 无有效记录。 */
    }

    u32PageEndAddress = m_flash_wl_page_end_addr(&area->cfg, page_index); /* 计算页结束地址。 */
    u32RecordAddress = m_flash_wl_page_addr(&area->cfg, page_index) + M_FLASH_WL_PAGE_HEADER_SIZE; /* 第一条记录从页头后开始。 */

    for(u16SlotIndex = 0U; u16SlotIndex < area->records_per_page; u16SlotIndex++) /* 扫描本页所有槽位。 */
    {
        if((u32RecordAddress + area->cfg.record_size) > u32PageEndAddress) /* 防止越过页边界。 */
        {
            break; /* 结束扫描。 */
        }

        if(m_flash_wl_is_blank(u32RecordAddress, area->cfg.record_size)) /* 遇到空白槽位，后面通常也为空。 */
        {
            break; /* 停止扫描。 */
        }

        if(m_flash_wl_record_valid(area, u32RecordAddress, NULL, NULL)) /* 当前槽位是一条完整有效记录。 */
        {
            u16ValidRecordCount++; /* 累加有效记录数。 */
        }

        u32RecordAddress += area->cfg.record_size; /* 移到下一条记录槽位。 */
    }

    return u16ValidRecordCount; /* 返回当前页有效记录数量。 */
}

/* 扫描单页，统计有效记录数量，找到本页第一个空白槽位，并更新全局最新记录地址。 */
static m_flash_wl_page_scan_t m_flash_wl_scan_page(m_flash_wl_area_t *area, uint16_t page_index)
{
    m_flash_wl_page_scan_t tPageScan;        /* 本页扫描结果。 */
    m_flash_wl_page_header_t tPageHeader;   /* 本页页头内容。 */
    uint32_t u32RecordAddress;              /* 当前扫描的记录槽位地址。 */
    uint32_t u32PageEndAddress;             /* 当前页结束地址。 */
    uint32_t u32RecordSequence;             /* 当前有效记录的记录序号。 */
    uint16_t u16SlotIndex;                  /* 记录槽位循环下标。 */

    tPageScan.valid = false; /* 默认认为页无效。 */
    tPageScan.page_seq = 0U; /* 默认页序号为0。 */
    tPageScan.first_free_addr = m_flash_wl_page_end_addr(&area->cfg, page_index); /* 默认空白地址放到页末。 */
    tPageScan.valid_count = 0U; /* 默认有效记录数量为0。 */

    if(!m_flash_wl_page_header_valid(&area->cfg, page_index, &tPageHeader)) /* 先检查页头。 */
    {
        return tPageScan; /* 页头无效，直接返回默认扫描结果。 */
    }

    tPageScan.valid = true;                 /* 页头有效，标记本页有效。 */
    tPageScan.page_seq = tPageHeader.page_seq; /* 保存本页页序号。 */
    u32PageEndAddress = m_flash_wl_page_end_addr(&area->cfg, page_index); /* 计算页结束地址。 */
    u32RecordAddress = m_flash_wl_page_addr(&area->cfg, page_index) + M_FLASH_WL_PAGE_HEADER_SIZE; /* 第一条记录从页头后开始。 */

    for(u16SlotIndex = 0U; u16SlotIndex < area->records_per_page; u16SlotIndex++) /* 逐个扫描本页记录槽位。 */
    {
        if((u32RecordAddress + area->cfg.record_size) > u32PageEndAddress) /* 如果当前槽位已经超过页边界。 */
        {
            break; /* 停止扫描，避免越界。 */
        }

        if(m_flash_wl_is_blank(u32RecordAddress, area->cfg.record_size)) /* 如果当前槽位整段都是空白。 */
        {
            tPageScan.first_free_addr = u32RecordAddress; /* 这里就是本页第一个可写地址。 */
            break;                       /* 后面槽位理论上也应为空，停止扫描。 */
        }

        if(m_flash_wl_record_valid(area, u32RecordAddress, &u32RecordSequence, NULL)) /* 如果当前槽位是一条有效记录。 */
        {
            tPageScan.valid_count++; /* 本页有效记录数量加1。 */
            if((area->latest_addr == M_FLASH_WL_INVALID_ADDR) || (u32RecordSequence >= area->next_record_seq)) /* 如果这是目前扫描到的最新记录。 */
            {
                area->latest_addr = u32RecordAddress; /* 更新最新记录地址。 */
                area->next_record_seq = u32RecordSequence; /* 临时保存最大记录序号，init最后会再加1变成下一条序号。 */
            }
        }

        tPageScan.first_free_addr = u32RecordAddress + area->cfg.record_size; /* 如果当前槽位非空，空白地址先假设为下一槽位。 */
        u32RecordAddress += area->cfg.record_size;                       /* 移动到下一条记录槽位。 */
    }

    return tPageScan; /* 返回本页扫描结果。 */
}

/* 空白分区第一次初始化时，只写第0页页头，不擦除整个分区。 */
static m_flash_wl_status_t m_flash_wl_create_first_page(m_flash_wl_area_t *area)
{
    m_flash_wl_status_t eCreatePageStatus; /* 保存Flash操作结果。 */

    eCreatePageStatus = m_flash_wl_write_page_header(&area->cfg, 0U, 0U, 0U); /* 在第0页写页头，页序号和擦除次数从0开始。 */

    if(eCreatePageStatus == M_FLASH_WL_OK) /* 如果页头写入成功。 */
    {
        area->current_page = 0U; /* 当前页设为第0页。 */
        area->page_seq = 0U; /* 当前页序号设为0。 */
        area->write_addr = area->cfg.start_addr + M_FLASH_WL_PAGE_HEADER_SIZE; /* 下一次写入从页头后开始。 */
    }

    return eCreatePageStatus; /* 返回创建结果。 */
}

/* 当前页写满后切换到下一页：擦除下一页，写新页头，然后从新页开始写。 */
static m_flash_wl_status_t m_flash_wl_switch_page(m_flash_wl_area_t *area)
{
    m_flash_wl_status_t eSwitchPageStatus;  /* 保存Flash操作结果。 */
    m_flash_wl_page_header_t tOldPageHeader; /* 如果下一页原本有页头，用于读取旧擦除次数。 */
    uint16_t u16TryPageCount;               /* 尝试页数量，最多尝试当前页之外的所有页。 */
    uint16_t u16NextPageIndex;              /* 准备切换到的下一页序号。 */
    uint16_t u16OldValidRecordCount;       /* 被覆盖页原本保存的有效记录数量。 */
    uint32_t u32EraseCount;                 /* 新页擦除次数。 */
    uint32_t u32PageAddress;                /* 新页起始地址。 */

    u16NextPageIndex = area->current_page; /* 从当前页开始往后找下一页。 */

    for(u16TryPageCount = 0U; u16TryPageCount < (area->cfg.page_count - 1U); u16TryPageCount++) /* 当前最新页不得作为切页目标。 */
    {
        u16NextPageIndex++; /* 先移动到下一页。 */
        if(u16NextPageIndex >= area->cfg.page_count) /* 如果超过最后一页。 */
        {
            u16NextPageIndex = 0U; /* 回到第0页，形成环形使用。 */
        }

        u16OldValidRecordCount = m_flash_wl_count_valid_records_on_page(area, u16NextPageIndex); /* 覆盖旧页前先统计旧记录数。 */
        u32EraseCount = 0U; /* 默认擦除次数从0开始。 */
        if(m_flash_wl_page_header_valid(&area->cfg, u16NextPageIndex, &tOldPageHeader)) /* 如果目标页原来有有效页头。 */
        {
            u32EraseCount = tOldPageHeader.erase_count + 1U; /* 在原擦除次数基础上加1。 */
        }

        u32PageAddress = m_flash_wl_page_addr(&area->cfg, u16NextPageIndex); /* 计算目标页地址。 */
        eSwitchPageStatus = m_flash_wl_erase_page(u32PageAddress); /* 擦除目标页。 */
        if(eSwitchPageStatus != M_FLASH_WL_OK) /* 只有擦除失败时才尝试下一个非当前页。 */
        {
            continue;
        }

        eSwitchPageStatus = m_flash_wl_write_page_header(&area->cfg, u16NextPageIndex, area->page_seq + 1U, u32EraseCount); /* 写入新页头，页序号加1。 */
        if(eSwitchPageStatus != M_FLASH_WL_OK) /* 擦除已成功，此时不能继续破坏其他有效页。 */
        {
            return eSwitchPageStatus;
        }

        area->current_page = u16NextPageIndex; /* 更新当前页。 */
        area->page_seq++; /* 更新当前页序号。 */
        area->write_addr = u32PageAddress + M_FLASH_WL_PAGE_HEADER_SIZE; /* 新页写指针指向页头后。 */
        if(area->valid_record_count >= u16OldValidRecordCount) /* 从总有效记录数中扣掉被覆盖页的旧记录。 */
        {
            area->valid_record_count -= u16OldValidRecordCount; /* 维持总条数等于当前Flash中真实有效记录数量。 */
        }
        else /* 防御计数异常。 */
        {
            area->valid_record_count = 0U; /* 重新从0开始累计后续新记录。 */
        }
        return M_FLASH_WL_OK; /* 切页成功。 */
    }

    return M_FLASH_WL_ERR_FLASH; /* 所有页都尝试失败，返回Flash错误。 */
}

/* 初始化均衡擦写分区的内部实现；调用者必须已经持有Flash资源守卫。 */
static m_flash_wl_status_t m_flash_wl_init_unlocked(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg)
{
    m_flash_wl_status_t eInitStatus;     /* 保存配置检查或Flash操作结果。 */
    m_flash_wl_page_scan_t tPageScan;    /* 单页扫描结果。 */
    uint16_t u16PageIndex;               /* 页循环下标。 */
    bool bValidPageFound = false;        /* 是否扫描到至少一个有效页。 */

    if(area == NULL) /* 运行状态对象不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    eInitStatus = m_flash_wl_validate_cfg(cfg); /* 检查分区配置是否合法。 */
    if(eInitStatus != M_FLASH_WL_OK) /* 如果配置检查失败。 */
    {
        return eInitStatus; /* 返回具体错误。 */
    }

    memset(area, 0, sizeof(*area)); /* 清空运行状态。 */
    area->cfg = *cfg; /* 保存一份配置到运行状态中。 */
    area->latest_addr = M_FLASH_WL_INVALID_ADDR; /* 初始化最新记录地址为无效。 */
    area->write_addr = M_FLASH_WL_INVALID_ADDR;  /* 初始化写指针为无效。 */
    area->records_per_page = m_flash_wl_calc_records_per_page(cfg->record_size); /* 计算每页记录数。 */
    area->payload_size = cfg->record_size - M_FLASH_WL_RECORD_HEADER_SIZE - M_FLASH_WL_RECORD_COMMIT_SIZE; /* 计算业务数据最大长度。 */

    for(u16PageIndex = 0U; u16PageIndex < cfg->page_count; u16PageIndex++) /* 扫描分区内每一页。 */
    {
        tPageScan = m_flash_wl_scan_page(area, u16PageIndex); /* 扫描当前页。 */
        if(tPageScan.valid) /* 如果当前页页头有效。 */
        {
            area->valid_record_count += tPageScan.valid_count; /* 累加有效记录数量。 */
            if((!bValidPageFound) || (tPageScan.page_seq >= area->page_seq)) /* 找页序号最大的页，认为它是当前最新页。 */
            {
                bValidPageFound = true;        /* 标记已经找到有效页。 */
                area->page_seq = tPageScan.page_seq;/* 保存最新页序号。 */
                area->current_page = u16PageIndex;     /* 保存当前页序号。 */
                area->write_addr = tPageScan.first_free_addr; /* 下一次写入从这个页的第一个空白槽开始。 */
            }
        }
    }

    if(!bValidPageFound) /* 如果一个有效页头都没有找到。 */
    {
        if(!m_flash_wl_is_area_blank(cfg)) /* 分区不是空白，但又没有有效页头。 */
        {
            return M_FLASH_WL_ERR_CRC; /* 认为分区数据损坏，不能自动擦除，避免误删旧数据。 */
        }

        eInitStatus = m_flash_wl_create_first_page(area); /* 空白分区第一次使用，创建第0页页头。 */
        if(eInitStatus != M_FLASH_WL_OK) /* 如果创建失败。 */
        {
            return eInitStatus; /* 返回错误。 */
        }
    }

    if(area->latest_addr != M_FLASH_WL_INVALID_ADDR) /* 如果扫描到了最新记录。 */
    {
        area->next_record_seq++; /* 扫描过程中next_record_seq临时保存最大序号，这里加1变成下一条记录序号。 */
    }
    else /* 如果当前分区还没有任何有效记录。 */
    {
        area->next_record_seq = 0U; /* 下一条记录从0开始编号。 */
    }

    area->initialized = true; /* 标记分区初始化完成。 */
    return M_FLASH_WL_OK; /* 初始化成功。 */
}

/* 初始化均衡擦写分区：整个扫描/首次建页过程必须保持Flash资源独占。 */
m_flash_wl_status_t m_flash_wl_init(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg)
{
    m_flash_wl_status_t eInitStatus;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    eInitStatus = m_flash_wl_init_unlocked(area, cfg);
    d_flash_release();
    return eInitStatus;
}

/* 格式化分区：擦除所有页，并重新创建第0页页头。 */
static m_flash_wl_status_t m_flash_wl_format_unlocked(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg)
{
    m_flash_wl_status_t eFormatStatus; /* 保存操作结果。 */
    uint16_t u16PageIndex;              /* 页循环下标。 */

    if(area == NULL) /* 运行状态对象不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    eFormatStatus = m_flash_wl_validate_cfg(cfg); /* 先检查配置是否合法。 */
    if(eFormatStatus != M_FLASH_WL_OK) /* 如果配置不合法。 */
    {
        return eFormatStatus; /* 返回具体错误。 */
    }

    for(u16PageIndex = 0U; u16PageIndex < cfg->page_count; u16PageIndex++) /* 遍历分区内所有页。 */
    {
        d_watchdog_feed(); /* 格式化会连续擦除多页，逐页喂狗。 */
        eFormatStatus = m_flash_wl_erase_page(m_flash_wl_page_addr(cfg, u16PageIndex)); /* 擦除当前页。 */
        if(eFormatStatus != M_FLASH_WL_OK) /* 如果任意一页擦除失败。 */
        {
            return eFormatStatus; /* 返回错误。 */
        }
    }

    memset(area, 0, sizeof(*area)); /* 擦完后清空运行状态。 */
    area->cfg = *cfg; /* 保存配置。 */
    area->records_per_page = m_flash_wl_calc_records_per_page(cfg->record_size); /* 重新计算每页记录数。 */
    area->payload_size = cfg->record_size - M_FLASH_WL_RECORD_HEADER_SIZE - M_FLASH_WL_RECORD_COMMIT_SIZE; /* 重新计算业务数据最大长度。 */
    area->latest_addr = M_FLASH_WL_INVALID_ADDR; /* 格式化后没有最新记录。 */
    area->write_addr = cfg->start_addr + M_FLASH_WL_PAGE_HEADER_SIZE; /* 写指针指向第0页页头后。 */
    area->current_page = 0U; /* 当前页为第0页。 */
    area->page_seq = 0U; /* 页序号从0开始。 */
    area->next_record_seq = 0U; /* 记录序号从0开始。 */

    eFormatStatus = m_flash_wl_write_page_header(cfg, 0U, 0U, 1U); /* 写第0页页头，格式化算作擦除过一次。 */

    if(eFormatStatus == M_FLASH_WL_OK) /* 如果页头写成功。 */
    {
        area->initialized = true; /* 标记初始化完成。 */
    }

    return eFormatStatus; /* 返回格式化结果。 */
}

/* 格式化分区：所有页擦除和页头重建作为一个Flash事务执行。 */
m_flash_wl_status_t m_flash_wl_format(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg)
{
    m_flash_wl_status_t eFormatStatus;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    eFormatStatus = m_flash_wl_format_unlocked(area, cfg);
    d_flash_release();
    return eFormatStatus;
}

/* 追加写入一条记录，采用“头、数据、提交标志”的顺序保证掉电安全。 */
static m_flash_wl_status_t m_flash_wl_append_unlocked(m_flash_wl_area_t *area, const uint8_t *data, uint16_t len)
{
    m_flash_wl_record_header_t tRecordHeader; /* RAM中的记录头。 */
    m_flash_wl_status_t eAppendStatus;        /* 保存操作结果。 */
    uint32_t u32CommitAddress;                /* 提交标志写入地址。 */
    uint32_t u32PageEndAddress;               /* 当前页结束地址。 */
    uint32_t u32RecordSlotAddress;            /* 本条记录使用的槽位起始地址。 */

    if((area == NULL) || (!area->initialized)) /* 分区状态不能为空且必须已经初始化。 */
    {
        return M_FLASH_WL_ERR_NOT_INIT; /* 返回未初始化错误。 */
    }

    if((len > area->payload_size) || ((data == NULL) && (len > 0U))) /* 数据长度不能超限，非零长度时data不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    while(true) /* 寻找一个可写的空白记录槽位。 */
    {
        u32PageEndAddress = m_flash_wl_page_end_addr(&area->cfg, area->current_page); /* 计算当前页结束地址。 */
        if((area->write_addr + area->cfg.record_size) > u32PageEndAddress) /* 如果当前页剩余空间放不下一条记录。 */
        {
            eAppendStatus = m_flash_wl_switch_page(area); /* 切换到下一页。 */
            if(eAppendStatus != M_FLASH_WL_OK) /* 如果切页失败。 */
            {
                return eAppendStatus; /* 返回错误。 */
            }
            continue; /* 切页后重新检查新页空间。 */
        }

        if(m_flash_wl_is_blank(area->write_addr, area->cfg.record_size)) /* 如果当前槽位是空白。 */
        {
            break; /* 找到可写槽位，退出循环。 */
        }

        area->write_addr += area->cfg.record_size; /* 当前槽位非空，跳到下一槽位。 */
    }

    u32RecordSlotAddress = area->write_addr; /* 记录本条记录要写入的槽位起始地址。 */
    memset(&tRecordHeader, 0xFF, sizeof(tRecordHeader)); /* 记录头先填0xFF，保持未用字节为擦除态。 */
    tRecordHeader.magic = M_FLASH_WL_RECORD_MAGIC; /* 写入记录魔术字。 */
    tRecordHeader.record_seq = area->next_record_seq; /* 写入当前记录序号。 */
    tRecordHeader.payload_len = len; /* 写入业务数据长度。 */
    tRecordHeader.payload_crc = m_flash_wl_crc16(data, len); /* 计算并写入业务数据CRC。 */
    tRecordHeader.record_size = area->cfg.record_size; /* 写入记录槽位大小。 */
    tRecordHeader.header_crc = m_flash_wl_crc16((const uint8_t *)&tRecordHeader,
                                          offsetof(m_flash_wl_record_header_t, header_crc)); /* 计算并写入记录头CRC。 */

    eAppendStatus = m_flash_wl_program_buffer(u32RecordSlotAddress, (const uint8_t *)&tRecordHeader, sizeof(tRecordHeader)); /* 第一步写记录头。 */
    if((eAppendStatus == M_FLASH_WL_OK) && (len > 0U)) /* 记录头写成功，并且有业务数据需要写。 */
    {
        eAppendStatus = m_flash_wl_program_buffer(u32RecordSlotAddress + M_FLASH_WL_RECORD_HEADER_SIZE, data, len); /* 第二步写业务数据。 */
    }
    if(eAppendStatus == M_FLASH_WL_OK) /* 记录头和业务数据都写成功。 */
    {
        u32CommitAddress = u32RecordSlotAddress + area->cfg.record_size - M_FLASH_WL_RECORD_COMMIT_SIZE; /* 提交标志地址在槽位最后4字节。 */
        eAppendStatus = m_flash_wl_program_word(u32CommitAddress, M_FLASH_WL_COMMIT_MAGIC); /* 第三步写提交标志，表示本条记录完整有效。 */
    }
    area->write_addr += area->cfg.record_size; /* 无论本条是否成功，都跳到下一槽位，避免覆盖半写记录。 */
    if(eAppendStatus != M_FLASH_WL_OK) /* 如果写入过程失败。 */
    {
        return eAppendStatus; /* 返回错误，半写记录由于没有提交标志或CRC错误，启动扫描时会被忽略。 */
    }

    area->latest_addr = u32RecordSlotAddress; /* 更新最新记录地址。 */
    area->next_record_seq++; /* 下一条记录序号加1。 */
    area->valid_record_count++; /* 有效记录总数加1。 */

    return M_FLASH_WL_OK; /* 追加写入成功。 */
}

/* 追加记录：记录头、数据和提交标志必须作为一个不可交错事务写入。 */
m_flash_wl_status_t m_flash_wl_append(m_flash_wl_area_t *area, const uint8_t *data, uint16_t len)
{
    m_flash_wl_status_t eAppendStatus;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    eAppendStatus = m_flash_wl_append_unlocked(area, data, len);
    d_flash_release();
    return eAppendStatus;
}

/* 读取当前分区最新一条有效记录。 */
static m_flash_wl_status_t m_flash_wl_read_latest_unlocked(const m_flash_wl_area_t *area, uint8_t *buffer,
                                           uint16_t buffer_len, uint16_t *out_len, uint32_t *out_seq)
{
    const m_flash_wl_record_header_t *ptRecordHeader; /* 指向Flash中的最新记录头。 */
    uint16_t u16PayloadLength;                       /* 最新记录业务数据长度。 */
    uint32_t u32RecordSequence;                      /* 最新记录序号。 */

    if((area == NULL) || (!area->initialized)) /* 分区状态不能为空且必须初始化。 */
    {
        return M_FLASH_WL_ERR_NOT_INIT; /* 返回未初始化错误。 */
    }

    if(area->latest_addr == M_FLASH_WL_INVALID_ADDR) /* 如果当前还没有任何有效记录。 */
    {
        return M_FLASH_WL_ERR_EMPTY; /* 返回空记录错误。 */
    }

    if(!m_flash_wl_record_valid(area, area->latest_addr, &u32RecordSequence, &u16PayloadLength)) /* 读取前再次校验最新记录。 */
    {
        return M_FLASH_WL_ERR_CRC; /* 校验失败，返回CRC错误。 */
    }

    if(buffer_len < u16PayloadLength) /* 调用者提供的buffer不够大。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    if((buffer == NULL) && (u16PayloadLength > 0U)) /* 有数据要读时buffer不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    ptRecordHeader = (const m_flash_wl_record_header_t *)area->latest_addr; /* 取最新记录头。 */
    if(u16PayloadLength > 0U) /* 如果这条记录有业务数据。 */
    {
        memcpy(buffer, (const void *)(area->latest_addr + M_FLASH_WL_RECORD_HEADER_SIZE), u16PayloadLength); /* 拷贝业务数据到调用者buffer。 */
    }

    if(out_len != NULL) /* 如果调用者需要输出长度。 */
    {
        *out_len = ptRecordHeader->payload_len; /* 输出业务数据长度。 */
    }

    if(out_seq != NULL) /* 如果调用者需要输出记录序号。 */
    {
        *out_seq = ptRecordHeader->record_seq; /* 输出记录序号。 */
    }

    return M_FLASH_WL_OK; /* 读取成功。 */
}

/* 读取最新记录时也持有Flash守卫，避免与擦写事务交错。 */
m_flash_wl_status_t m_flash_wl_read_latest(const m_flash_wl_area_t *area, uint8_t *buffer,
                                           uint16_t buffer_len, uint16_t *out_len, uint32_t *out_seq)
{
    m_flash_wl_status_t eReadStatus;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    eReadStatus = m_flash_wl_read_latest_unlocked(area, buffer, buffer_len, out_len, out_seq);
    d_flash_release();
    return eReadStatus;
}

/* 按有效记录索引读取一条记录；index从1开始，1是最旧记录，valid_record_count是最新记录。 */
static m_flash_wl_status_t m_flash_wl_read_by_index_unlocked(const m_flash_wl_area_t *area, uint16_t index,
                                             uint8_t *buffer, uint16_t buffer_len,
                                             uint16_t *out_len, uint32_t *out_seq)
{
    uint32_t u32OldestRecordSequence; /* 当前分区内最旧有效记录的序号。 */
    uint32_t u32TargetRecordSequence; /* 本次要读取的目标记录序号。 */
    uint32_t u32RecordSequence;       /* 扫描到的记录序号。 */
    uint32_t u32RecordAddress;        /* 当前扫描的记录槽位地址。 */
    uint32_t u32PageEndAddress;       /* 当前页结束地址。 */
    uint16_t u16PayloadLength;        /* 目标记录业务数据长度。 */
    uint16_t u16PageIndex;            /* 页循环下标。 */
    uint16_t u16SlotIndex;            /* 页内槽位循环下标。 */

    if((area == NULL) || (!area->initialized)) /* 分区状态不能为空，并且必须已经初始化。 */
    {
        return M_FLASH_WL_ERR_NOT_INIT; /* 返回未初始化错误。 */
    }

    if(area->valid_record_count == 0UL) /* 当前分区还没有任何有效记录。 */
    {
        return M_FLASH_WL_ERR_EMPTY; /* 返回空记录错误。 */
    }

    if((index == 0U) || ((uint32_t)index > area->valid_record_count) ||
       ((buffer == NULL) && (buffer_len > 0U))) /* index采用1起始，不能超过有效记录总数。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    u32OldestRecordSequence = area->next_record_seq - area->valid_record_count; /* 序号连续递增，所以可由下一条序号反推最旧序号。 */
    u32TargetRecordSequence = u32OldestRecordSequence + (uint32_t)(index - 1U); /* 1号对应最旧序号，count号对应最新序号。 */

    for(u16PageIndex = 0U; u16PageIndex < area->cfg.page_count; u16PageIndex++) /* 扫描分区内每一页。 */
    {
        if(!m_flash_wl_page_header_valid(&area->cfg, u16PageIndex, NULL)) /* 跳过无效页。 */
        {
            continue; /* 当前页不属于本分区或页头损坏，继续下一页。 */
        }

        u32PageEndAddress = m_flash_wl_page_end_addr(&area->cfg, u16PageIndex); /* 计算当前页结束地址。 */
        u32RecordAddress = m_flash_wl_page_addr(&area->cfg, u16PageIndex) + M_FLASH_WL_PAGE_HEADER_SIZE; /* 第一条记录在页头之后。 */

        for(u16SlotIndex = 0U; u16SlotIndex < area->records_per_page; u16SlotIndex++) /* 扫描当前页的每一个记录槽位。 */
        {
            if((u32RecordAddress + area->cfg.record_size) > u32PageEndAddress) /* 防止超过页边界。 */
            {
                break; /* 当前页扫描结束。 */
            }

            if(m_flash_wl_is_blank(u32RecordAddress, area->cfg.record_size)) /* 后续槽位理论上也为空。 */
            {
                break; /* 当前页已经没有更多记录。 */
            }

            if(m_flash_wl_record_valid(area, u32RecordAddress, &u32RecordSequence, &u16PayloadLength) && (u32RecordSequence == u32TargetRecordSequence)) /* 找到目标序号。 */
            {
                if(buffer_len < u16PayloadLength) /* 调用者提供的buffer必须能放下业务数据。 */
                {
                    return M_FLASH_WL_ERR_PARAM; /* buffer太小，返回参数错误。 */
                }

                if((buffer == NULL) && (u16PayloadLength > 0U)) /* 非零长度记录必须提供buffer。 */
                {
                    return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
                }

                if(u16PayloadLength > 0U) /* 目标记录有业务数据。 */
                {
                    memcpy(buffer, (const void *)(u32RecordAddress + M_FLASH_WL_RECORD_HEADER_SIZE), u16PayloadLength); /* 拷贝业务数据。 */
                }

                if(out_len != NULL) /* 调用者需要长度。 */
                {
                    *out_len = u16PayloadLength; /* 输出业务数据长度。 */
                }

                if(out_seq != NULL) /* 调用者需要底层记录序号。 */
                {
                    *out_seq = u32RecordSequence; /* 输出记录序号。 */
                }

                return M_FLASH_WL_OK; /* 读取成功。 */
            }

            u32RecordAddress += area->cfg.record_size; /* 继续扫描下一条槽位。 */
        }
    }

    return M_FLASH_WL_ERR_EMPTY; /* 有效记录计数和实际扫描不一致时，按未找到处理。 */
}

/* 按索引读取记录时持有Flash守卫，保证扫描期间不会被写事务改变。 */
m_flash_wl_status_t m_flash_wl_read_by_index(const m_flash_wl_area_t *area, uint16_t index,
                                             uint8_t *buffer, uint16_t buffer_len,
                                             uint16_t *out_len, uint32_t *out_seq)
{
    m_flash_wl_status_t eReadStatus;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    eReadStatus = m_flash_wl_read_by_index_unlocked(area, index, buffer, buffer_len, out_len, out_seq);
    d_flash_release();
    return eReadStatus;
}

/* 获取单条记录可保存的最大业务数据长度。 */
uint16_t m_flash_wl_get_payload_size(const m_flash_wl_area_t *area)
{
    if((area == NULL) || (!area->initialized)) /* 分区状态不能为空且必须初始化。 */
    {
        return 0U; /* 不可用时返回0。 */
    }

    return area->payload_size; /* 返回payload可用长度。 */
}

/* 获取当前分区运行状态，方便上层Watch或调试输出。 */
m_flash_wl_status_t m_flash_wl_get_info(const m_flash_wl_area_t *area, m_flash_wl_info_t *info)
{
    if((area == NULL) || (info == NULL)) /* 输入参数不能为空。 */
    {
        return M_FLASH_WL_ERR_PARAM; /* 返回参数错误。 */
    }

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return M_FLASH_WL_ERR_FLASH;
    }

    info->start_addr = area->cfg.start_addr;            /* 输出分区起始地址。 */
    info->write_addr = area->write_addr;                /* 输出下一次写入地址。 */
    info->latest_addr = area->latest_addr;              /* 输出最新记录地址。 */
    info->page_seq = area->page_seq;                    /* 输出当前页序号。 */
    info->next_record_seq = area->next_record_seq;      /* 输出下一条记录序号。 */
    info->valid_record_count = area->valid_record_count;/* 输出有效记录总数。 */
    info->current_page = area->current_page;            /* 输出当前页下标。 */
    info->page_count = area->cfg.page_count;            /* 输出分区页数量。 */
    info->record_size = area->cfg.record_size;          /* 输出每条记录槽位大小。 */
    info->records_per_page = area->records_per_page;    /* 输出每页记录数。 */
    info->payload_size = area->payload_size;            /* 输出业务数据最大长度。 */
    info->initialized = area->initialized;              /* 输出初始化状态。 */

    d_flash_release();
    return M_FLASH_WL_OK; /* 状态获取成功。 */
}

