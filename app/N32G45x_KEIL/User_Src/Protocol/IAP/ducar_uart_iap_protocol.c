/*
 * 文件作用：
 *   DUCAR UART IAP协议和通用升级包的唯一实现入口，同时承载UART升级业务。
 *
 * 负责内容：
 *   1. UART IAP帧解析/组帧、CRC32、固件品号和升级元数据校验。
 *   2. APP2擦写、固件接收、校验、JUMP及升级状态维护。
 *   3. UART协议输入、BMS/IAP命令分流和在线状态维护。
 */
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#include "Protocol/Comm/ducar_uart_protocol.h"

#include "App/app_bms_fault.h"
#include "App/app_current_calib.h"
#include "App/app_flash_map.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "Driver/d_timer.h"
#include "Driver/d_uart.h"
#include "Driver/d_flash.h"
#include "Driver/d_system.h"
#include "Driver/d_watchdog.h"
#include <stddef.h>
#include <string.h>

typedef enum
{
    DUCAR_IAP_STATE_WAIT_SOF = 0U, /* 等待帧头0xAA。 */
    DUCAR_IAP_STATE_ADDR,          /* 接收地址0x01。 */
    DUCAR_IAP_STATE_CMD,           /* 接收命令字。 */
    DUCAR_IAP_STATE_LEN,           /* 接收DATA长度。 */
    DUCAR_IAP_STATE_DATA,          /* 接收DATA内容。 */
    DUCAR_IAP_STATE_SUM,           /* 接收单字节累加校验和。 */
    DUCAR_IAP_STATE_EOF0,          /* 接收帧尾0x3A。 */
    DUCAR_IAP_STATE_EOF1           /* 接收帧尾0x4B。 */
} ducar_iap_state_t;

/* 从数据区读取一个小端无符号16位值。 */
static uint16_t ducar_iap_read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

/* 从数据区读取一个小端无符号32位值。 */
static uint32_t ducar_iap_read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/* 复位IAP帧解析器，清空当前帧状态并等待新的帧头。 */
static void ducar_iap_parser_reset(ducar_iap_parser_t *parser)
{
    parser->state = (uint8_t)DUCAR_IAP_STATE_WAIT_SOF;
    parser->data_index = 0U;
    parser->checksum = 0U;
    parser->frame.cmd = 0U;
    parser->frame.len = 0U;
}

/* 处理IAP帧解析错误，保留命令字并复位解析器。 */
static ducar_iap_parse_result_t ducar_iap_parser_error(ducar_iap_parser_t *parser, ducar_iap_frame_t *out_frame)
{
    if (out_frame != 0)
    {
        *out_frame = parser->frame; /* 保留命令字，方便上层按原命令回复NAK。 */
    }

    ducar_iap_parser_reset(parser);
    return DUCAR_IAP_PARSE_ERROR;
}

/* 初始化IAP协议解析器。 */
void ducar_iap_parser_init(ducar_iap_parser_t *parser)
{
    if (parser == 0)
    {
        return;
    }

    ducar_iap_parser_reset(parser);
}

/* 输入一个字节并推进IAP协议帧解析状态机。 */
ducar_iap_parse_result_t ducar_iap_parser_input(ducar_iap_parser_t *parser, uint8_t byte, ducar_iap_frame_t *out_frame)
{
    if (parser == 0)
    {
        return DUCAR_IAP_PARSE_NONE;
    }

    switch ((ducar_iap_state_t)parser->state)
    {
        case DUCAR_IAP_STATE_WAIT_SOF:
            if (byte == DUCAR_IAP_SOF)
            {
                ducar_iap_parser_reset(parser);
                parser->checksum = DUCAR_IAP_SOF;
                parser->state = (uint8_t)DUCAR_IAP_STATE_ADDR;
            }
            break;

        case DUCAR_IAP_STATE_ADDR:
            if (byte != DUCAR_IAP_ADDR)
            {
                ducar_iap_parser_reset(parser);
                if (byte == DUCAR_IAP_SOF)
                {
                    parser->checksum = DUCAR_IAP_SOF;
                    parser->state = (uint8_t)DUCAR_IAP_STATE_ADDR;
                }
                break;
            }

            parser->checksum = (uint8_t)(DUCAR_IAP_SOF + DUCAR_IAP_ADDR);
            parser->state = (uint8_t)DUCAR_IAP_STATE_CMD;
            break;

        case DUCAR_IAP_STATE_CMD:
            parser->frame.cmd = byte;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            parser->state = (uint8_t)DUCAR_IAP_STATE_LEN;
            break;

        case DUCAR_IAP_STATE_LEN:
            parser->frame.len = byte;
            parser->data_index = 0U;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->frame.len == 0U)
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_SUM;
            }
            else
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_DATA;
            }
            break;

        case DUCAR_IAP_STATE_DATA:
            parser->frame.data[parser->data_index] = byte;
            parser->data_index++;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->data_index >= parser->frame.len)
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_SUM;
            }
            break;

        case DUCAR_IAP_STATE_SUM:
            if (byte != parser->checksum)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_IAP_STATE_EOF0;
            break;

        case DUCAR_IAP_STATE_EOF0:
            if (byte != DUCAR_IAP_EOF0)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_IAP_STATE_EOF1;
            break;

        case DUCAR_IAP_STATE_EOF1:
            if (byte != DUCAR_IAP_EOF1)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            if (out_frame != 0)
            {
                *out_frame = parser->frame;
            }
            ducar_iap_parser_reset(parser);
            return DUCAR_IAP_PARSE_OK;

        default:
            ducar_iap_parser_reset(parser);
            break;
    }

    return DUCAR_IAP_PARSE_NONE;
}

/* 按照IAP协议格式组装一帧待发送数据。 */
uint16_t ducar_iap_pack_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out_frame, uint16_t out_size)
{
    uint8_t u8Checksum;
    uint16_t u16FrameIndex;
    uint8_t u8DataIndex;

    if ((out_frame == 0) || (out_size < (uint16_t)(7U + len)) || ((data == 0) && (len > 0U)))
    {
        return 0U;
    }

    u16FrameIndex = 0U;
    u8Checksum = (uint8_t)(DUCAR_IAP_SOF + DUCAR_IAP_ADDR);

    out_frame[u16FrameIndex++] = DUCAR_IAP_SOF;
    out_frame[u16FrameIndex++] = DUCAR_IAP_ADDR;

    out_frame[u16FrameIndex++] = cmd;
    u8Checksum = (uint8_t)(u8Checksum + cmd);

    out_frame[u16FrameIndex++] = len;
    u8Checksum = (uint8_t)(u8Checksum + len);

    for (u8DataIndex = 0U; u8DataIndex < len; u8DataIndex++)
    {
        out_frame[u16FrameIndex++] = data[u8DataIndex];
        u8Checksum = (uint8_t)(u8Checksum + data[u8DataIndex]);
    }

    out_frame[u16FrameIndex++] = u8Checksum;
    out_frame[u16FrameIndex++] = DUCAR_IAP_EOF0;
    out_frame[u16FrameIndex++] = DUCAR_IAP_EOF1;

    return u16FrameIndex;
}

/* 从Program帧中读取小端帧序号。 */
uint16_t ducar_iap_get_frame_no(const ducar_iap_frame_t *frame)
{
    if ((frame == 0) || (frame->len < 2U))
    {
        return 0xFFFFU;
    }

    return ducar_iap_read_le16(&frame->data[0]);
}

/* 解析Update Start请求中的页数量和页大小。 */
bool ducar_iap_parse_update_start(const ducar_iap_frame_t *frame, ducar_iap_update_info_t *info)
{
    if ((frame == 0) || (info == 0) || (frame->cmd != DUCAR_IAP_CMD_UPDATE_START) || (frame->len != 4U))
    {
        return false;
    }

    info->page_count = ducar_iap_read_le16(&frame->data[0]);
    info->page_size = ducar_iap_read_le16(&frame->data[2]);
    return true;
}

/* 解析Program元数据中的固件CRC、长度和品号。 */
bool ducar_iap_parse_firmware_info(const ducar_iap_frame_t *frame, ducar_iap_firmware_info_t *info)
{
    const uint8_t mark[DUCAR_IAP_META_MARK_LEN] = {'D', 'U', 'C', 'A', 'R', 0xFFU, 0xFFU};
    const uint8_t *meta;

    if ((frame == 0) || (info == 0) || (frame->cmd != DUCAR_IAP_CMD_PROGRAM) ||
        (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN) || (ducar_iap_get_frame_no(frame) != 0U))
    {
        return false;
    }

    meta = &frame->data[2];
    if (memcmp(&meta[DUCAR_IAP_META_MARK_OFFSET], mark, DUCAR_IAP_META_MARK_LEN) != 0)
    {
        return false;
    }

    memset(info, 0, sizeof(*info));
    memcpy(info->part_number, &meta[DUCAR_IAP_META_PART_OFFSET], DUCAR_IAP_META_PART_LEN);
    info->part_number[DUCAR_IAP_META_PART_LEN] = '\0';
    info->crc32 = ducar_iap_read_le32(&meta[DUCAR_IAP_META_CRC_OFFSET]);
    info->size = ducar_iap_read_le32(&meta[DUCAR_IAP_META_SIZE_OFFSET]);
    return true;
}

/* 校验固件品号是否允许在当前设备上升级。 */
bool ducar_iap_part_number_is_allowed(const char *local_part_number, const char *firmware_part_number)
{
    if ((local_part_number == 0) || (firmware_part_number == 0))
    {
        return false;
    }

    /*
     * A000000000是未定义品号时的强制升级通配品号。
     * 上位机使用该品号出包时，BOOT和APP都允许继续升级。
     */
    if (strncmp(firmware_part_number, DUCAR_IAP_DEFAULT_PART_NUMBER, DUCAR_IAP_META_PART_LEN) == 0)
    {
        return true;
    }

    return (strncmp(firmware_part_number, local_part_number, DUCAR_IAP_META_PART_LEN) == 0);
}

/* 根据固件长度计算实际Program数据帧数量。 */
uint16_t ducar_iap_calc_data_frames(uint32_t firmware_size)
{
    if (firmware_size == 0UL)
    {
        return 0U;
    }

    return (uint16_t)((firmware_size + (uint32_t)DUCAR_IAP_PAGE_SIZE - 1UL) / (uint32_t)DUCAR_IAP_PAGE_SIZE);
}

/* 计算固件数据使用的CRC32校验值。 */
uint32_t ducar_iap_crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t u32Crc32 = 0xFFFFFFFFUL;
    uint32_t u32ByteIndex;
    uint8_t u8BitIndex;

    if ((data == 0) && (len > 0UL))
    {
        return 0UL;
    }

    for (u32ByteIndex = 0UL; u32ByteIndex < len; u32ByteIndex++)
    {
        u32Crc32 ^= data[u32ByteIndex];
        for (u8BitIndex = 0U; u8BitIndex < 8U; u8BitIndex++)
        {
            if ((u32Crc32 & 1UL) != 0UL)
            {
                u32Crc32 = (u32Crc32 >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                u32Crc32 >>= 1;
            }
        }
    }

    return ~u32Crc32;
}


/* APP内执行升级接收：固件写入APP2备份区，Jump命令后置CCCC标志并复位进BOOT搬运。 */
#define APP_IAP_BOOT_DB_START_ADDR        APP_FLASH_MAP_BOOT_DB_START_ADDR
#define APP_IAP_BOOT_DB_END_ADDR          APP_FLASH_MAP_BOOT_DB_END_ADDR
#define APP_IAP_UPGRADE_FLAG_ADDR         APP_FLASH_MAP_BOOT_UPGRADE_FLAG_ADDR
#define APP_IAP_DIRECT_DEBUG_VECTOR_ADDR  APP_FLASH_MAP_FLASH_START_ADDR
#define APP_IAP_VTOR_ADDR_MASK            ((uint32_t)0x1FFFFF80UL)
#define APP_IAP_UPGRADE_FLAG_DEFAULT      ((uint32_t)0xFFFFFFFFUL)
#define APP_IAP_UPGRADE_FLAG_APP_VALID    ((uint32_t)0xAAAAAAAAUL)
#define APP_IAP_UPGRADE_FLAG_WAIT_PACKAGE ((uint32_t)0xBBBBBBBBUL)
#define APP_IAP_UPGRADE_FLAG_NEED_MOVE    ((uint32_t)0xCCCCCCCCUL)
#define APP_IAP_UPGRADE_INFO_ADDR         APP_FLASH_MAP_BOOT_UPGRADE_INFO_ADDR
#define APP_IAP_UPGRADE_INFO_MAGIC        APP_FLASH_MAP_BOOT_UPGRADE_INFO_MAGIC
#define APP_IAP_UPGRADE_INFO_SIZE         APP_FLASH_MAP_BOOT_UPGRADE_INFO_SIZE
#define APP_IAP_APP1_START_ADDR           APP_FLASH_MAP_APP1_START_ADDR
#define APP_IAP_APP_CODE_SIZE             APP_FLASH_MAP_APP_CODE_SIZE
#define APP_IAP_APP1_END_ADDR             APP_FLASH_MAP_APP1_END_ADDR
#define APP_IAP_APP2_START_ADDR           APP_FLASH_MAP_APP2_START_ADDR
#define APP_IAP_APP2_END_ADDR             APP_FLASH_MAP_APP2_END_ADDR
#define APP_IAP_SRAM_START_ADDR           ((uint32_t)0x20000000UL)
#define APP_IAP_SRAM_SIZE                 ((uint32_t)0x00024000UL)
#define APP_IAP_SRAM_END_ADDR             (APP_IAP_SRAM_START_ADDR + APP_IAP_SRAM_SIZE)

typedef enum
{
    APP_IAP_STATE_IDLE = 0U,      /* 未进入升级，等待Update Start。 */
    APP_IAP_STATE_READY,          /* 已收到Update Start，等待第0帧元数据。 */
    APP_IAP_STATE_RECEIVING,      /* 正在按帧号顺序写APP2。 */
    APP_IAP_STATE_FINISHED        /* APP2已经收完整并通过CRC校验。 */
} ducar_iap_upgrade_state_t;

static ducar_iap_upgrade_state_t s_ducar_iap_state;            /* APP侧升级接收状态。 */
static ducar_iap_update_info_t s_ducar_iap_update_info;        /* Update Start里的页数量和页大小。 */
static ducar_iap_firmware_info_t s_ducar_iap_fw_info;          /* 第0帧解析出的固件信息。 */
static uint16_t s_ducar_iap_expected_frame;                 /* 下一帧期望收到的Program帧号。 */
static uint16_t s_ducar_iap_total_data_frames;              /* 固件真实数据需要的Program帧数，不含第0帧。 */
static uint8_t s_ducar_iap_backup_ready;                    /* APP2备份固件是否已通过校验。 */
static uint8_t s_ducar_iap_tx_frame[DUCAR_IAP_MAX_FRAME_LEN];  /* 应答帧发送缓冲。 */

typedef struct
{
    uint32_t magic;                 /* 元数据有效标识。 */
    uint32_t image_size;            /* APP2中有效固件区的实际长度。 */
    uint32_t image_crc32;           /* APP2中有效固件区的整包CRC32。 */
    uint32_t record_crc32;          /* 对前面三个字段计算的元数据CRC32。 */
} app_iap_boot_upgrade_info_t;

/* 擦除APP与BOOT共享的升级数据库页。 */
static bool ducar_iap_erase_boot_db(void)
{
    return d_flash_erase_page(APP_IAP_BOOT_DB_START_ADDR);
}

/* 在数据库页已经擦除后写入升级状态标志。 */
static bool ducar_iap_write_upgrade_flag(uint32_t flag)
{
    if (flag == APP_IAP_UPGRADE_FLAG_DEFAULT)
    {
        return true;
    }

    return d_flash_write(APP_IAP_UPGRADE_FLAG_ADDR,
                          (const uint8_t *)&flag,
                          (uint32_t)sizeof(flag));
}

/* 写入APP与BOOT共享的升级状态标志。 */
static bool ducar_iap_set_upgrade_flag(uint32_t flag)
{
    if ((flag != APP_IAP_UPGRADE_FLAG_DEFAULT) &&
        (flag != APP_IAP_UPGRADE_FLAG_APP_VALID) &&
        (flag != APP_IAP_UPGRADE_FLAG_WAIT_PACKAGE) &&
        (flag != APP_IAP_UPGRADE_FLAG_NEED_MOVE))
    {
        return false;
    }

    if (!ducar_iap_erase_boot_db())
    {
        return false;
    }

    return ducar_iap_write_upgrade_flag(flag);
}

/* 读取APP与BOOT共享的升级状态标志。 */
static uint32_t ducar_iap_get_upgrade_flag(void)
{
    /* APP只读BOOT数据区最后4字节，用它判断当前升级状态。 */
    return *((volatile const uint32_t *)APP_IAP_UPGRADE_FLAG_ADDR);
}

/* 判断当前运行镜像是否为直接调试镜像。 */
static bool ducar_iap_is_direct_debug_image(void)
{
    uint32_t u32VectorTableAddress;
    uint32_t u32StackAddress;
    uint32_t u32ResetAddress;

    /*
     * 兼容直接运行在0x08000000的旧调试镜像。
     * 当前正式APP链接到0x08010000；只有检测到旧调试向量表时，
     * 才禁止擦写BOOT数据页，避免旧镜像覆盖或擦除自身运行区域。
     */
    u32VectorTableAddress = (d_system_get_vector_table_addr() & APP_IAP_VTOR_ADDR_MASK);
    if (u32VectorTableAddress != APP_IAP_DIRECT_DEBUG_VECTOR_ADDR)
    {
        return false;
    }

    u32StackAddress = *((volatile const uint32_t *)APP_IAP_DIRECT_DEBUG_VECTOR_ADDR);
    u32ResetAddress = *((volatile const uint32_t *)(APP_IAP_DIRECT_DEBUG_VECTOR_ADDR + 4UL));

    if ((u32StackAddress < APP_IAP_SRAM_START_ADDR) ||
        (u32StackAddress > APP_IAP_SRAM_END_ADDR) ||
        ((u32StackAddress & 0x03UL) != 0UL))
    {
        return false;
    }

    return ((u32ResetAddress >= APP_IAP_DIRECT_DEBUG_VECTOR_ADDR) &&
            (u32ResetAddress < APP_IAP_BOOT_DB_START_ADDR) &&
            ((u32ResetAddress & 0x01UL) != 0UL));
}

/* 在默认标志状态下确认当前APP有效。 */
static void ducar_iap_mark_app_valid_if_default(void)
{
    /*
     * 擦除态0xFFFFFFFF只表示默认上电时BOOT可以尝试跳APP。
     * APP真正运行起来后，把标志写成APP_VALID，后续CHECK才能确认不是旧APP卡住。
     */
    if (ducar_iap_is_direct_debug_image())
    {
        return;
    }

    if (ducar_iap_get_upgrade_flag() == APP_IAP_UPGRADE_FLAG_DEFAULT)
    {
        (void)ducar_iap_set_upgrade_flag(APP_IAP_UPGRADE_FLAG_APP_VALID);
    }
}

/* 擦除APP2升级备份区域。 */
static bool ducar_iap_erase_app2(void)
{
    uint32_t u32PageAddress;

    for (u32PageAddress = APP_IAP_APP2_START_ADDR;
         u32PageAddress < APP_IAP_APP2_END_ADDR;
         u32PageAddress += D_FLASH_PAGE_SIZE)
    {
        d_watchdog_feed();
        if (!d_flash_erase_page(u32PageAddress))
        {
            return false;
        }
    }

    return true;
}

/* 校验备份镜像的栈地址是否有效。 */
static bool ducar_iap_stack_is_valid(uint32_t stack_addr)
{
    return ((stack_addr >= APP_IAP_SRAM_START_ADDR) &&
            (stack_addr <= APP_IAP_SRAM_END_ADDR) &&
            ((stack_addr & 0x03UL) == 0UL));
}

/* 校验备份镜像的复位向量是否有效。 */
static bool ducar_iap_reset_is_valid(uint32_t reset_addr)
{
    return ((reset_addr >= APP_IAP_APP1_START_ADDR) &&
            (reset_addr < APP_IAP_APP1_END_ADDR) &&
            ((reset_addr & 0x01UL) != 0UL));
}

/* 校验APP2备份镜像的向量表。 */
static bool ducar_iap_backup_vector_is_valid(void)
{
    uint32_t u32StackAddress = *((volatile const uint32_t *)APP_IAP_APP2_START_ADDR);
    uint32_t u32ResetAddress = *((volatile const uint32_t *)(APP_IAP_APP2_START_ADDR + 4UL));

    return (ducar_iap_stack_is_valid(u32StackAddress) && ducar_iap_reset_is_valid(u32ResetAddress));
}

/* 校验APP2备份镜像的长度和CRC32。 */
static bool ducar_iap_backup_crc_is_valid(void)
{
    uint32_t u32CalculatedCrc32;

    if ((s_ducar_iap_fw_info.size == 0UL) || (s_ducar_iap_fw_info.size > APP_IAP_APP_CODE_SIZE))
    {
        return false;
    }

    u32CalculatedCrc32 = ducar_iap_crc32_calc((const uint8_t *)APP_IAP_APP2_START_ADDR,
                                               s_ducar_iap_fw_info.size);
    return (u32CalculatedCrc32 == s_ducar_iap_fw_info.crc32);
}

/* 综合校验APP2备份镜像是否可搬运。 */
static bool ducar_iap_backup_is_valid(void)
{
    return (ducar_iap_backup_vector_is_valid() && ducar_iap_backup_crc_is_valid());
}

/* 校验Update Start参数是否符合当前升级约束。 */
static bool ducar_iap_update_info_is_valid(const ducar_iap_update_info_t *info)
{
    uint16_t u16MaximumPageCount =
        (uint16_t)((APP_IAP_APP_CODE_SIZE / (uint32_t)DUCAR_IAP_PAGE_SIZE) + 1UL);

    if (info == 0)
    {
        return false;
    }

    return ((info->page_size == DUCAR_IAP_PAGE_SIZE) &&
            (info->page_count >= 2U) &&
            (info->page_count <= u16MaximumPageCount));
}

/* 校验声明页数量与实际数据帧数量是否匹配。 */
static bool ducar_iap_page_count_matches(uint16_t update_page_count, uint16_t data_frames)
{
    /*
     * 协议示例里Update Start的页数量包含第0帧，所以应等于data_frames+1。
     * 为了兼容上位机只填写真实数据帧数的情况，这里也接受data_frames。
     */
    return ((update_page_count == (uint16_t)(data_frames + 1U)) ||
            (update_page_count == data_frames));
}

/* 复位APP IAP升级状态机及其缓存状态。 */
static void ducar_iap_reset_update_state(void)
{
    s_ducar_iap_state = APP_IAP_STATE_IDLE;
    s_ducar_iap_expected_frame = 0U;
    s_ducar_iap_total_data_frames = 0U;
    s_ducar_iap_backup_ready = 0U;
    memset(&s_ducar_iap_update_info, 0, sizeof(s_ducar_iap_update_info));
    memset(&s_ducar_iap_fw_info, 0, sizeof(s_ducar_iap_fw_info));
}

/* 通过当前G45x UART传输通道发送IAP协议帧。 */
static bool ducar_iap_send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint16_t u16FrameLength;

    u16FrameLength = ducar_iap_pack_frame(cmd,
                                          data,
                                          len,
                                          s_ducar_iap_tx_frame,
                                          (uint16_t)sizeof(s_ducar_iap_tx_frame));
    if (u16FrameLength == 0U)
    {
        return false;
    }

    return d_uart_try_send_buffer(s_ducar_iap_tx_frame, u16FrameLength);
}

/* 发送一个带单字节ACK或NAK状态的IAP响应。 */
static void ducar_iap_send_status(uint8_t cmd, uint8_t status)
{
    ducar_iap_send_frame(cmd, &status, 1U);
}

/* 生成并发送APP Check In响应。 */
static void ducar_iap_send_check_in(void)
{
    uint8_t au8ResponseData[2];

    au8ResponseData[0] = DUCAR_IAP_ACK;
    au8ResponseData[1] = DUCAR_IAP_IN_APP;
    ducar_iap_send_frame(DUCAR_IAP_CMD_CHECK_IN,
                         au8ResponseData,
                         (uint8_t)sizeof(au8ResponseData));
}

/* 处理Check命令并返回当前APP有效性。 */
static void ducar_iap_handle_check(void)
{
    /*
     * CHECK用于判断升级闭环是否真正完成：
     * 1. 0xCCCCCCCC表示APP已请求BOOT搬运，如果旧APP没复位出去，这里必须回NAK；
     * 2. 0xBBBBBBBB表示BOOT等待升级包，也不能认为APP运行成功；
     * 3. 只有APP已经被标记为0xAAAAAAAA有效运行态，APP才回ACK。
     */
    if (ducar_iap_get_upgrade_flag() == APP_IAP_UPGRADE_FLAG_APP_VALID)
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_CHECK, DUCAR_IAP_ACK);
    }
    else
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_CHECK, DUCAR_IAP_NAK);
    }
}

/* 处理Update Start命令并准备APP2接收区域。 */
static void ducar_iap_handle_update_start(const ducar_iap_frame_t *frame)
{
    ducar_iap_update_info_t tUpdateInfo;

    if ((!ducar_iap_parse_update_start(frame, &tUpdateInfo)) ||
        (!ducar_iap_update_info_is_valid(&tUpdateInfo)))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    ducar_iap_reset_update_state();
    if (!ducar_iap_erase_app2())
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_NAK);
        return;
    }

    s_ducar_iap_update_info = tUpdateInfo;
    s_ducar_iap_state = APP_IAP_STATE_READY;
    ducar_iap_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_ACK);
}

/* 处理Program元数据帧并建立固件接收上下文。 */
static void ducar_iap_handle_program_info(const ducar_iap_frame_t *frame)
{
    ducar_iap_firmware_info_t tFirmwareInfo;
    uint16_t u16DataFrameCount;

    if ((s_ducar_iap_state != APP_IAP_STATE_READY) ||
        (!ducar_iap_parse_firmware_info(frame, &tFirmwareInfo)) ||
        (!ducar_iap_part_number_is_allowed(app_param_get_software_pn(), tFirmwareInfo.part_number)) ||
        (tFirmwareInfo.size == 0UL) || (tFirmwareInfo.size > APP_IAP_APP_CODE_SIZE))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    u16DataFrameCount = ducar_iap_calc_data_frames(tFirmwareInfo.size);
    if ((u16DataFrameCount == 0U) ||
        (!ducar_iap_page_count_matches(s_ducar_iap_update_info.page_count, u16DataFrameCount)))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    s_ducar_iap_fw_info = tFirmwareInfo;
    s_ducar_iap_total_data_frames = u16DataFrameCount;
    s_ducar_iap_expected_frame = 1U;
    s_ducar_iap_state = APP_IAP_STATE_RECEIVING;
    ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_ACK);
}

/* 按序校验并写入一帧固件数据。 */
static void ducar_iap_handle_program_data(const ducar_iap_frame_t *frame, uint16_t frame_no)
{
    uint32_t u32PayloadOffset;
    uint32_t u32RemainingLength;
    uint32_t u32WriteLength;

    if ((s_ducar_iap_state != APP_IAP_STATE_RECEIVING) || (frame_no != s_ducar_iap_expected_frame) ||
        (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    u32PayloadOffset = ((uint32_t)frame_no - 1UL) * (uint32_t)DUCAR_IAP_PAGE_SIZE;
    if (u32PayloadOffset >= s_ducar_iap_fw_info.size)
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    u32RemainingLength = s_ducar_iap_fw_info.size - u32PayloadOffset;
    u32WriteLength = (u32RemainingLength > (uint32_t)DUCAR_IAP_PAGE_SIZE) ?
                     (uint32_t)DUCAR_IAP_PAGE_SIZE : u32RemainingLength;
    if (!d_flash_write(APP_IAP_APP2_START_ADDR + u32PayloadOffset,
                       &frame->data[2],
                       u32WriteLength))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    if (frame_no >= s_ducar_iap_total_data_frames)
    {
        if (!ducar_iap_backup_is_valid())
        {
            ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
            ducar_iap_reset_update_state();
            return;
        }

        s_ducar_iap_backup_ready = 1U;
        s_ducar_iap_state = APP_IAP_STATE_FINISHED;
    }
    else
    {
        s_ducar_iap_expected_frame++;
    }

    ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_ACK);
}

/* 分发Program元数据帧和固件数据帧。 */
static void ducar_iap_handle_program(const ducar_iap_frame_t *frame)
{
    uint16_t u16FrameNumber;

    if ((frame == 0) || (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    u16FrameNumber = ducar_iap_get_frame_no(frame);
    if (u16FrameNumber == 0U)
    {
        ducar_iap_handle_program_info(frame);
    }
    else
    {
        ducar_iap_handle_program_data(frame, u16FrameNumber);
    }
}

/* 处理Jump命令，校验镜像并设置BOOT搬运标志。 */
static void ducar_iap_handle_jump(void)
{
    uint8_t u8AckStatus = DUCAR_IAP_ACK;

    if (ducar_iap_is_direct_debug_image())
    {
        /*
         * 直接调试版没有真正从BOOT运行，写CCCC标志会擦到当前APP镜像尾部。
         * 因此这里明确拒绝JUMP，完整升级跳转需要把APP恢复到0x0800C000工程配置后再测试。
         */
        ducar_iap_send_status(DUCAR_IAP_CMD_JUMP, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    if ((s_ducar_iap_state != APP_IAP_STATE_FINISHED) || (s_ducar_iap_backup_ready == 0U) || (!ducar_iap_backup_is_valid()))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_JUMP, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    if (!ducar_iap_prepare_boot_move(s_ducar_iap_fw_info.size,
                                     s_ducar_iap_fw_info.crc32))
    {
        ducar_iap_send_status(DUCAR_IAP_CMD_JUMP, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
        return;
    }

    if (ducar_iap_send_frame(DUCAR_IAP_CMD_JUMP, &u8AckStatus, 1U))
    {
        if (!d_uart_wait_tx_complete())
        {
            return;
        }
        d_timer_delay_ms(2U); /* 多留一点点时间给上位机收到ACK，再复位进入BOOT搬运。 */
        d_system_reset();
    }
}

/* 判断命令字是否属于IAP协议。 */
bool ducar_iap_is_protocol_cmd(uint8_t cmd)
{
    return ((cmd == DUCAR_IAP_CMD_CHECK_IN) ||
            (cmd == DUCAR_IAP_CMD_UPDATE_START) ||
            (cmd == DUCAR_IAP_CMD_PROGRAM) ||
            (cmd == DUCAR_IAP_CMD_JUMP) ||
            (cmd == DUCAR_IAP_CMD_CHECK));
}

/* 处理一帧完整的IAP业务请求。 */
void ducar_iap_handle_frame(const ducar_iap_frame_t *frame)
{
    if (frame == 0)
    {
        return;
    }

    switch (frame->cmd)
    {
        case DUCAR_IAP_CMD_CHECK_IN:
            ducar_iap_send_check_in();
            break;

        case DUCAR_IAP_CMD_UPDATE_START:
            ducar_iap_handle_update_start(frame);
            break;

        case DUCAR_IAP_CMD_PROGRAM:
            ducar_iap_handle_program(frame);
            break;

        case DUCAR_IAP_CMD_JUMP:
            ducar_iap_handle_jump();
            break;

        case DUCAR_IAP_CMD_CHECK:
            ducar_iap_handle_check();
            break;

        default:
            break;
    }
}

/* 处理IAP帧校验或格式错误并返回NAK。 */
void ducar_iap_handle_parse_error(uint8_t cmd)
{
    if ((cmd == DUCAR_IAP_CMD_UPDATE_START) ||
        (cmd == DUCAR_IAP_CMD_PROGRAM) ||
        (cmd == DUCAR_IAP_CMD_JUMP) ||
        (cmd == DUCAR_IAP_CMD_CHECK))
    {
        ducar_iap_send_status(cmd, DUCAR_IAP_NAK);
        ducar_iap_reset_update_state();
    }
}

/* 保存搬运所需的镜像元数据，并准备复位进入BOOT搬运。 */
bool ducar_iap_prepare_boot_move(uint32_t image_size, uint32_t image_crc32)
{
    app_iap_boot_upgrade_info_t tUpgradeInfo;

    /* APP升级使用同一套PF保存和BOOT搬运标志。 */
    if (ducar_iap_is_direct_debug_image())
    {
        return false;
    }

    if (app_bms_fault_save_shutdown_pf() == 0U)
    {
        return false;
    }

    if ((image_size < 8UL) || (image_size > APP_IAP_APP_CODE_SIZE))
    {
        return false;
    }

    tUpgradeInfo.magic = APP_IAP_UPGRADE_INFO_MAGIC;
    tUpgradeInfo.image_size = image_size;
    tUpgradeInfo.image_crc32 = image_crc32;
    tUpgradeInfo.record_crc32 = ducar_iap_crc32_calc(
        (const uint8_t *)&tUpgradeInfo,
        (uint32_t)offsetof(app_iap_boot_upgrade_info_t, record_crc32));

    if (!ducar_iap_erase_boot_db())
    {
        return false;
    }

    if (!d_flash_write(APP_IAP_UPGRADE_INFO_ADDR,
                       (const uint8_t *)&tUpgradeInfo,
                       APP_IAP_UPGRADE_INFO_SIZE))
    {
        return false;
    }

    return ducar_iap_write_upgrade_flag(APP_IAP_UPGRADE_FLAG_NEED_MOVE);
}

/* 保存故障掉电记录并把BOOT置为等待CAN升级包；这里不提前擦除APP2。 */
bool ducar_iap_prepare_boot_wait_package(void)
{
    if (ducar_iap_is_direct_debug_image())
    {
        return false;
    }

    if (app_bms_fault_save_shutdown_pf() == 0U)
    {
        return false;
    }

    return ducar_iap_set_upgrade_flag(APP_IAP_UPGRADE_FLAG_WAIT_PACKAGE);
}

/* UDS交接肯定响应未能提交时恢复APP有效标志。 */
bool ducar_iap_cancel_boot_wait_package(void)
{
    if (ducar_iap_get_upgrade_flag() != APP_IAP_UPGRADE_FLAG_WAIT_PACKAGE)
    {
        return false;
    }
    return ducar_iap_set_upgrade_flag(APP_IAP_UPGRADE_FLAG_APP_VALID);
}

 

/* 判断APP IAP升级状态机是否处于忙状态。 */
bool ducar_iap_is_busy(void)
{
    return (s_ducar_iap_state != APP_IAP_STATE_IDLE);
}

/* 复制IAP解析器、元数据和收包状态，供调试窗口查看升级卡在哪一步。 */



#define DUCAR_IAP_PROTOCOL_ONLINE_TIMEOUT_MS ((uint32_t)5000UL)

static ducar_iap_parser_t s_ducar_iap_protocol_parser;
static uint8_t s_ducar_iap_protocol_online;
static uint32_t s_ducar_iap_protocol_last_rx_ms;

/* 根据最近UART接收活动更新协议在线状态。 */
static void ducar_iap_protocol_update_online_state(void)
{
    if ((s_ducar_iap_protocol_online != 0U) &&
        ((uint32_t)(d_timer_get_ms() - s_ducar_iap_protocol_last_rx_ms) >= DUCAR_IAP_PROTOCOL_ONLINE_TIMEOUT_MS))
    {
        s_ducar_iap_protocol_online = 0U;
    }
}

/* 记录一次UART接收活动并刷新在线计时。 */
static void ducar_iap_protocol_mark_rx_activity(void)
{
    s_ducar_iap_protocol_online = 1U;
    s_ducar_iap_protocol_last_rx_ms = d_timer_get_ms();
}

/* 初始化IAP协议、升级状态和UART在线状态。 */
void ducar_iap_protocol_init(void)
{
    ducar_iap_parser_init(&s_ducar_iap_protocol_parser);
    ducar_iap_reset_update_state();
    ducar_iap_mark_app_valid_if_default();
    s_ducar_iap_protocol_online = 0U;
    s_ducar_iap_protocol_last_rx_ms = 0UL;
}

/* 查询并返回UART协议在线状态。 */
uint8_t ducar_iap_protocol_is_online(void)
{
    ducar_iap_protocol_update_online_state();
    return s_ducar_iap_protocol_online;
}

/* 将UART中的普通BMS帧转换后交给BMS协议模块。 */
static void ducar_iap_protocol_dispatch_bms_frame(const ducar_iap_frame_t *frame)
{
    ducar_bms_frame_t tBmsFrame;

    if (frame == 0)
    {
        return;
    }

    if (frame->len > DUCAR_BMS_MAX_DATA_LEN)
    {
        ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
        return;
    }

    tBmsFrame.cmd = frame->cmd;
    tBmsFrame.len = frame->len;
    if (frame->len > 0U)
    {
        memcpy(tBmsFrame.data, frame->data, frame->len);
    }

    ducar_bms_handle_frame(&tBmsFrame);
}

/* 执行UART协议解析、BMS/IAP分流和日志导出任务。 */
void ducar_iap_protocol_task(void)
{
    uint8_t u8ReceivedByte;
    ducar_iap_frame_t tIapFrame;
    ducar_iap_parse_result_t eParseResult;

    ducar_iap_protocol_update_online_state();

    while (d_uart_read_byte(&u8ReceivedByte) != 0U)
    {
        /* 收到串口字节即认为上位机在线，是否为合法协议帧由后续解析流程判断。 */
        ducar_iap_protocol_mark_rx_activity();

        /*
         * 新协议下普通通讯和升级通讯都使用AA 01 CMD LEN DATA SUM 3A 4B。
         * 因此串口层只做一次完整帧解析，再根据CMD分发到IAP或普通BMS业务。
         */
        eParseResult = ducar_iap_parser_input(&s_ducar_iap_protocol_parser,
                                              u8ReceivedByte,
                                              &tIapFrame);
        if (eParseResult == DUCAR_IAP_PARSE_OK)
        {
            if (ducar_iap_is_protocol_cmd(tIapFrame.cmd))
            {
                if (app_current_calib_cal2_is_active())
                {
                    ducar_iap_send_status(tIapFrame.cmd, DUCAR_IAP_NAK);
                }
                else
                {
                    ducar_iap_handle_frame(&tIapFrame);
                }
            }
            else if (!ducar_iap_is_busy())
            {
                ducar_iap_protocol_dispatch_bms_frame(&tIapFrame);
            }
            else
            {
                ducar_bms_send_status(tIapFrame.cmd, DUCAR_BMS_NAK);
            }
        }
        else if (eParseResult == DUCAR_IAP_PARSE_ERROR)
        {
            if (ducar_iap_is_protocol_cmd(tIapFrame.cmd))
            {
                ducar_iap_handle_parse_error(tIapFrame.cmd);
            }
        }
        else
        {
            /* 当前帧还没收完整，继续等待后续字节。 */
        }
    }

    ducar_iap_protocol_update_online_state();
    ducar_bms_uart_log_task(); /* 串口接收处理完以后，每次任务最多补发一条LOG数据。 */
}
