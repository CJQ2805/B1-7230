/*
 * DUCAR CAN IAP protocol on APP side.
 *
 * APP侧负责CAN升级入口确认、升级包接收、APP2暂存、首包头解析和整包CRC校验。
 * 完整镜像校验成功后，APP才写入搬运标志并复位进入BOOT；BOOT侧有独立的
 * CAN升级/搬运处理模块，用于BOOT自身升级或在APP不可用时继续接收升级流程。
 * 本文件只实现APP侧状态机，不改变普通CAN协议的报文分发。
 */
#include "Protocol/IAP/ducar_can_iap_protocol.h"

#include <string.h>

#include "App/app_flash_map.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "Driver/d_flash.h"
#include "Driver/d_system.h"
#include "Driver/d_timer.h"
#include "Driver/d_watchdog.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"

#define DUCAR_CAN_IAP_DATA_DLC                 ((uint8_t)8U)
/* Upgrade session expires after five minutes without a valid IAP frame. */
#define DUCAR_CAN_IAP_TIMEOUT_MS              ((uint32_t)300000UL)
#define DUCAR_CAN_IAP_TX_RETRY_TICKS          ((uint8_t)20U)
#define DUCAR_CAN_IAP_TP_RTS                  ((uint8_t)0x10U)
#define DUCAR_CAN_IAP_TP_CTS                  ((uint8_t)0x11U)
#define DUCAR_CAN_IAP_TP_EOM                  ((uint8_t)0x13U)
#define DUCAR_CAN_IAP_TP_ABORT                ((uint8_t)0xFFU)
#define DUCAR_CAN_IAP_UPGRADE_FLAG_APP_VALID  ((uint32_t)0xAAAAAAAAUL)
#define DUCAR_CAN_IAP_APP1_START_ADDR         APP_FLASH_MAP_APP1_START_ADDR
#define DUCAR_CAN_IAP_APP1_END_ADDR           APP_FLASH_MAP_APP1_END_ADDR
#define DUCAR_CAN_IAP_APP2_START_ADDR         APP_FLASH_MAP_APP2_START_ADDR
#define DUCAR_CAN_IAP_APP2_END_ADDR           APP_FLASH_MAP_APP2_END_ADDR
#define DUCAR_CAN_IAP_SRAM_START_ADDR         ((uint32_t)0x20000000UL)
#define DUCAR_CAN_IAP_SRAM_SIZE               ((uint32_t)0x00024000UL)
#define DUCAR_CAN_IAP_SRAM_END_ADDR           (DUCAR_CAN_IAP_SRAM_START_ADDR + DUCAR_CAN_IAP_SRAM_SIZE)

typedef enum
{
    DUCAR_CAN_IAP_STATE_IDLE = 0U,
    DUCAR_CAN_IAP_STATE_WAIT_PAGE,
    DUCAR_CAN_IAP_STATE_RECEIVING_PAGE,
    DUCAR_CAN_IAP_STATE_RESET_PENDING
} ducar_can_iap_state_t;

typedef enum
{
    DUCAR_CAN_IAP_TX_IDLE = 0U,
    DUCAR_CAN_IAP_TX_PENDING,
    DUCAR_CAN_IAP_TX_WAIT_IDLE
} ducar_can_iap_tx_state_t;

static ducar_can_iap_state_t eCanIapState;
static ducar_can_iap_tx_state_t eCanIapTxState;
static uint32_t u32CanIapExpectedFirmwareSize;
static uint32_t u32CanIapPackageSize;
static uint32_t u32CanIapPackageReceived;
static uint32_t u32CanIapFirmwareReceived;
static ducar_iap_firmware_info_t tCanIapFirmwareInfo;
static uint8_t u8CanIapHeaderReceived;
static uint16_t u16CanIapPageLength;
static uint8_t u8CanIapPagePackets;
static uint8_t u8CanIapExpectedPacket;
static uint8_t u8CanIapHostAddress;
static uint32_t u32CanIapLastActivityMs;
static uint8_t u8CanIapTxRetryTicks;
static uint8_t u8CanIapMoveAfterTx;
static t_can_packet tCanIapTxPacket;
static uint8_t au8CanIapPageBuffer[DUCAR_CAN_IAP_PAGE_MESSAGE_LEN];

bool ducar_can_iap_protocol_is_busy(void)
{
    return (eCanIapState != DUCAR_CAN_IAP_STATE_IDLE) ||
           (eCanIapTxState != DUCAR_CAN_IAP_TX_IDLE);
}

/* 按DUCAR扩展帧格式组装CAN IAP使用的18Exxxxx类报文ID。 */
static uint32_t ducar_can_iap_build_id(uint8_t u8Pf,
                                       uint8_t u8Ps,
                                       uint8_t u8Sa)
{
    /* 普通CAN和IAP统一使用优先级6，回复ID为18Exxxxx。 */
    /* PF决定E1/ACK/TP控制或TP数据类型。 */
    /* PS写入目标地址。 */
    /* SA写入BMS_A源地址。 */
    return ((((uint32_t)DUCAR_CAN_DEFAULT_PRIORITY & 0x07UL) << 26U) |
            ((uint32_t)u8Pf << 16U) |
            ((uint32_t)u8Ps << 8U) |
            (uint32_t)u8Sa) & DUCAR_CAN_EXT_ID_MASK;
}

/* 从扩展帧ID中取PF：PF决定报文是E1控制、TP连接管理还是TP数据。 */
static uint8_t ducar_can_iap_get_pf(const t_can_packet *ptCanPacket)
{
    /* 扩展ID的16~23位是PF。 */
    return (uint8_t)((ptCanPacket->u32id >> 16U) & 0xFFUL);
}

/* 从扩展帧ID中取PS：PS是目标地址。 */
static uint8_t ducar_can_iap_get_ps(const t_can_packet *ptCanPacket)
{
    /* 扩展ID的8~15位是PS目标地址。 */
    return (uint8_t)((ptCanPacket->u32id >> 8U) & 0xFFUL);
}

/* 从扩展帧ID中取SA：SA是源地址，升级主机通常为PC地址。 */
static uint8_t ducar_can_iap_get_sa(const t_can_packet *ptCanPacket)
{
    /* 扩展ID的0~7位是SA源地址。 */
    return (uint8_t)(ptCanPacket->u32id & 0xFFUL);
}

/* 读取协议定义的小端16位字段，例如页长度。 */
static uint16_t ducar_can_iap_read_u16_le(const uint8_t *pu8Data)
{
    /* Data0为低字节，Data1为高字节。 */
    return (uint16_t)((uint16_t)pu8Data[0] |
                      ((uint16_t)pu8Data[1] << 8U));
}

/* 读取协议定义的小端32位字段，例如E1/02携带的固件总长度。 */
static uint32_t ducar_can_iap_read_u32_le(const uint8_t *pu8Data)
{
    /* 依次拼接4个小端字节，供E1/02固件长度解析使用。 */
    return ((uint32_t)pu8Data[0]) |
           ((uint32_t)pu8Data[1] << 8U) |
           ((uint32_t)pu8Data[2] << 16U) |
           ((uint32_t)pu8Data[3] << 24U);
}

/* 根据固件长度计算“128字节升级头 + 按页补齐后的固件区”总长度。 */
static uint32_t ducar_can_iap_calc_package_size(uint32_t u32FirmwareSize)
{
    uint32_t u32FirmwarePageCount;

    /* 固件长度向上取整为128字节页数。 */
    u32FirmwarePageCount = (u32FirmwareSize + DUCAR_CAN_IAP_PAGE_SIZE - 1UL) /
                           DUCAR_CAN_IAP_PAGE_SIZE;
    /* 总包长度由128字节升级头和对齐后的固件页组成。 */
    return DUCAR_CAN_IAP_PACKAGE_HEADER_SIZE +
           (u32FirmwarePageCount * DUCAR_CAN_IAP_PAGE_SIZE);
}

/* 清除当前CAN升级接收会话，回到等待下一次E1/02的空闲状态。 */
static void ducar_can_iap_reset_transfer_state(void)
{
    /* 接收状态回到IDLE，等待下一次E1/02。 */
    eCanIapState = DUCAR_CAN_IAP_STATE_IDLE;
    /* 清除主机声明的固件长度。 */
    u32CanIapExpectedFirmwareSize = 0UL;
    /* 清除根据首页头部计算的总包长度。 */
    u32CanIapPackageSize = 0UL;
    /* 清除已接收的升级包字节数。 */
    u32CanIapPackageReceived = 0UL;
    /* 清除已写入APP2的固件字节数。 */
    u32CanIapFirmwareReceived = 0UL;
    /* 清除上一包的固件头信息。 */
    memset(&tCanIapFirmwareInfo, 0, sizeof(tCanIapFirmwareInfo));
    /* 标记当前还没有解析首页头。 */
    u8CanIapHeaderReceived = 0U;
    /* 清除当前页长度。 */
    u16CanIapPageLength = 0U;
    /* 清除当前页DT数量。 */
    u8CanIapPagePackets = 0U;
    /* 清除期待的DT序号。 */
    u8CanIapExpectedPacket = 0U;
    /* 默认主机地址为PC，收到实际帧后再更新。 */
    u8CanIapHostAddress = DUCAR_CAN_PC_ADDRESS;
    /* 清除最后活动时间。 */
    u32CanIapLastActivityMs = 0UL;
    /* 页缓存恢复为FF，便于最后一页补齐。 */
    memset(au8CanIapPageBuffer, 0xFF, sizeof(au8CanIapPageBuffer));
}

/* 清除APP侧待发送的ACK/CTS/EOM/ABORT及其发送后动作状态。 */
static void ducar_can_iap_clear_tx_state(void)
{
    /* 发送状态回到IDLE。 */
    eCanIapTxState = DUCAR_CAN_IAP_TX_IDLE;
    /* 清除发送重试计数。 */
    u8CanIapTxRetryTicks = 0U;
    /* 清除发送完成后是否进入BOOT的标志。 */
    u8CanIapMoveAfterTx = 0U;
    /* 清除缓存帧，防止旧应答残留。 */
    memset(&tCanIapTxPacket, 0, sizeof(tCanIapTxPacket));
}

/* 将一帧IAP应答放入单帧发送状态机，实际写CAN邮箱由周期任务执行。 */
static bool ducar_can_iap_queue_tx(uint8_t u8Pf,
                                   uint8_t u8Destination,
                                   const uint8_t *pu8Data,
                                   uint8_t u8MoveAfterTx)
{
    /* 发送状态机同一时间只允许缓存一帧。 */
    if ((eCanIapTxState != DUCAR_CAN_IAP_TX_IDLE) ||
        /* 数据指针不能为空。 */
        (pu8Data == 0) ||
        /* E1应答必须点对点发送，不能使用全局地址。 */
        (u8Destination == DUCAR_CAN_GLOBAL_ADDRESS))
    {
        return false;
    }

    /* 清空发送帧对象，确保未使用字段有确定值。 */
    memset(&tCanIapTxPacket, 0, sizeof(tCanIapTxPacket));
    /* 按PF、目标地址和BMS源地址生成应答ID。 */
    tCanIapTxPacket.u32id = ducar_can_iap_build_id(u8Pf,
                                                   u8Destination,
                                                   DUCAR_CAN_BMS_A_ADDRESS);
    /* IAP协议固定使用29位扩展帧。 */
    tCanIapTxPacket.u32idType = D_CAN_ID_EXTENDED;
    /* 所有IAP控制应答固定使用8字节数据区。 */
    tCanIapTxPacket.u8len = DUCAR_CAN_IAP_DATA_DLC;
    /* 复制调用方准备好的8字节协议数据。 */
    memcpy(tCanIapTxPacket.au8data, pu8Data, DUCAR_CAN_IAP_DATA_DLC);
    /* 保存EOM发送完成后是否写搬运标志。 */
    u8CanIapMoveAfterTx = u8MoveAfterTx;
    /* 本次发送从0开始重试。 */
    u8CanIapTxRetryTicks = 0U;
    /* 由周期任务实际调用d_can_send。 */
    eCanIapTxState = DUCAR_CAN_IAP_TX_PENDING;
    return true;
}

/* 组装E1/01、E1/02、E1/03对应的ACK/NAK应答。 */
static bool ducar_can_iap_queue_e1_response(uint8_t u8Status,
                                            uint8_t u8Destination)
{
    uint8_t au8CanData[DUCAR_CAN_IAP_DATA_DLC];

    /* 先填充协议未使用字节。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0返回ACK或NAK。 */
    au8CanData[0] = u8Status;
    /* Data1标识当前响应来自APP区域。 */
    au8CanData[1] = DUCAR_IAP_IN_APP;
    /* Data5~Data7回填被确认功能PF=E1。 */
    au8CanData[5] = 0x00U;
    au8CanData[6] = DUCAR_CAN_PF_BOOT_CONTROL;
    au8CanData[7] = 0x00U;
    /* 交给统一单帧发送状态机。 */
    return ducar_can_iap_queue_tx(DUCAR_CAN_PF_ACK,
                                  u8Destination,
                                  au8CanData,
                                  0U);
}

/* 对一页DF00数据回复CTS，告知主机本页允许发送的DT数量和起始序号。 */
static bool ducar_can_iap_queue_cts(void)
{
    uint8_t au8CanData[DUCAR_CAN_IAP_DATA_DLC];

    /* CTS未使用字节按FF填充。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0=CTS。 */
    au8CanData[0] = DUCAR_CAN_IAP_TP_CTS;
    /* Data1返回本页DT总包数。 */
    au8CanData[1] = u8CanIapPagePackets;
    /* Data2指定从DT序号1开始。 */
    au8CanData[2] = 0x01U;
    /* Data5~Data7指向DF00升级PGN。 */
    au8CanData[5] = 0x00U;
    au8CanData[6] = (uint8_t)(DUCAR_CAN_IAP_PGN >> 8U);
    au8CanData[7] = 0x00U;
    /* 缓存CTS，等待协议任务提交CAN。 */
    return ducar_can_iap_queue_tx(DUCAR_CAN_PF_TP_CM,
                                  u8CanIapHostAddress,
                                  au8CanData,
                                  0U);
}

/* 对一页数据回复EOM；整包校验通过时同时登记发送完成后的BOOT搬运动作。 */
static bool ducar_can_iap_queue_eom(uint8_t u8MoveAfterTx)
{
    uint8_t au8CanData[DUCAR_CAN_IAP_DATA_DLC];

    /* EOM未使用字段按FF填充。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0=EOM。 */
    au8CanData[0] = DUCAR_CAN_IAP_TP_EOM;
    /* Data1~Data2返回当前页长度。 */
    au8CanData[1] = (uint8_t)(u16CanIapPageLength & 0xFFU);
    au8CanData[2] = (uint8_t)(u16CanIapPageLength >> 8U);
    /* Data3返回当前页DT包数。 */
    au8CanData[3] = u8CanIapPagePackets;
    /* Data5~Data7指向DF00升级PGN。 */
    au8CanData[5] = 0x00U;
    au8CanData[6] = (uint8_t)(DUCAR_CAN_IAP_PGN >> 8U);
    au8CanData[7] = 0x00U;
    /* 根据完整镜像校验结果记录EOM发送后是否准备进入BOOT。 */
    return ducar_can_iap_queue_tx(DUCAR_CAN_PF_TP_CM,
                                  u8CanIapHostAddress,
                                  au8CanData,
                                  u8MoveAfterTx);
}

/* 当前页格式、页序或Flash处理失败时，向主机发送TP ABORT并结束会话。 */
static bool ducar_can_iap_queue_abort(void)
{
    uint8_t au8CanData[DUCAR_CAN_IAP_DATA_DLC];

    /* ABORT未使用字段按FF填充。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0=ABORT。 */
    au8CanData[0] = DUCAR_CAN_IAP_TP_ABORT;
    /* Data5~Data7指向被终止的DF00会话。 */
    au8CanData[5] = 0x00U;
    au8CanData[6] = (uint8_t)(DUCAR_CAN_IAP_PGN >> 8U);
    au8CanData[7] = 0x00U;
    /* 缓存ABORT，等待周期任务发送。 */
    return ducar_can_iap_queue_tx(DUCAR_CAN_PF_TP_CM,
                                  u8CanIapHostAddress,
                                  au8CanData,
                                  0U);
}

/* 判断TP连接管理帧是否明确指向升级PGN DF00。 */
static bool ducar_can_iap_is_df00_control(const t_can_packet *ptCanPacket)
{
    /* 空指针和非8字节帧不可能是合法DF00连接管理报文。 */
    if ((ptCanPacket == 0) ||
        (ptCanPacket->u8len != DUCAR_CAN_IAP_DATA_DLC) ||
        (ptCanPacket->au8data[5] != 0x00U) ||
        (ptCanPacket->au8data[6] != (uint8_t)(DUCAR_CAN_IAP_PGN >> 8U)) ||
        (ptCanPacket->au8data[7] != 0x00U))
    {
        return false;
    }

    /* PF/PS/SA合法且PGN字段匹配，确认是DF00控制帧。 */
    return true;
}

/* 判断报文是否来自PC且目标为当前BMS，防止其他节点伪造升级控制。 */
static bool ducar_can_iap_is_pc_to_bms(const t_can_packet *ptCanPacket)
{
    /* 同时检查指针、源地址和目标地址。 */
    return ((ptCanPacket != 0) &&
            (ducar_can_iap_get_sa(ptCanPacket) == DUCAR_CAN_PC_ADDRESS) &&
            (ducar_can_iap_get_ps(ptCanPacket) == DUCAR_CAN_BMS_A_ADDRESS));
}

/* 校验E1/02长度和APP代码区容量，拒绝零长度、越界长度。 */
static bool ducar_can_iap_firmware_size_is_valid(uint32_t u32FirmwareSize)
{
    /* 固件至少需要向量表8字节，最大不能超过APP代码区。 */
    return ((u32FirmwareSize >= 8UL) &&
            (u32FirmwareSize <= APP_FLASH_MAP_APP_CODE_SIZE));
}

/* 首个升级页验证通过后擦除APP2暂存区，后续页面写入该区域。 */
static bool ducar_can_iap_erase_app2(void)
{
    uint32_t u32FlashAddress;

    /* 从APP2起始页开始逐页擦除到APP2结束地址。 */
    for (u32FlashAddress = DUCAR_CAN_IAP_APP2_START_ADDR;
         u32FlashAddress < DUCAR_CAN_IAP_APP2_END_ADDR;
         u32FlashAddress += APP_FLASH_MAP_FLASH_PAGE_SIZE)
    {
        /* Flash擦除可能耗时，先喂狗避免系统复位。 */
        d_watchdog_feed();
        /* 任意一页擦除失败都立即终止升级。 */
        if (!d_flash_erase_page(u32FlashAddress))
        {
            return false;
        }
    }

    return true;
}

/* 解析第一页中的128字节升级头，核对固件长度、型号，并准备APP2写入。 */
static bool ducar_can_iap_parse_first_page(void)
{
    ducar_iap_frame_t tIapFrame;
    ducar_iap_firmware_info_t tFirmwareInfo;

    /* 构造一个与UART IAP解析器兼容的PROGRAM帧对象。 */
    memset(&tIapFrame, 0, sizeof(tIapFrame));
    /* 指定这是固件PROGRAM数据。 */
    tIapFrame.cmd = DUCAR_IAP_CMD_PROGRAM;
    /* 复用IAP头解析器要求的数据长度。 */
    tIapFrame.len = DUCAR_IAP_PROGRAM_DATA_LEN;
    /* 首页CAN缓存前4字节是页地址，从第4字节开始才是升级头。 */
    memcpy(&tIapFrame.data[2],
           &au8CanIapPageBuffer[DUCAR_CAN_IAP_PAGE_DATA_OFFSET],
           DUCAR_CAN_IAP_PACKAGE_HEADER_SIZE);

    /* 先解析魔数、型号、固件长度和CRC等头部字段。 */
    if (!ducar_iap_parse_firmware_info(&tIapFrame, &tFirmwareInfo))
    {
        return false;
    }
    /* 固件长度必须同时满足容量限制和E1/02主机声明值。 */
    if (!ducar_can_iap_firmware_size_is_valid(tFirmwareInfo.size) ||
        (tFirmwareInfo.size != u32CanIapExpectedFirmwareSize) ||
        !ducar_iap_part_number_is_allowed(app_param_get_software_pn(),
                                           tFirmwareInfo.part_number))
    {
        return false;
    }
    /* 只有首页头校验通过后才允许擦除APP2。 */
    if (!ducar_can_iap_erase_app2())
    {
        return false;
    }

    /* 保存已验证的固件头，供后续页写入和最终CRC校验使用。 */
    tCanIapFirmwareInfo = tFirmwareInfo;
    /* 计算整个升级包包含多少个128字节页。 */
    u32CanIapPackageSize = ducar_can_iap_calc_package_size(tFirmwareInfo.size);
    /* 标记首页头已收到，后续页面按普通固件数据处理。 */
    u8CanIapHeaderReceived = 1U;
    return true;
}

/* 校验当前页地址是否与已接收页数连续，防止乱序或重复页写入。 */
static bool ducar_can_iap_page_address_is_expected(uint32_t u32PageAddress,
                                                   uint32_t *pu32PageOffset)
{
    uint32_t u32PageOffset;

    /* 输出指针和包总长度都必须有效。 */
    if ((pu32PageOffset == 0) || (u32CanIapPackageSize == 0UL))
    {
        return false;
    }
    /* 主机既可能发送APP1绝对地址，也可能发送包内相对偏移。 */
    if ((u32PageAddress >= DUCAR_CAN_IAP_APP1_START_ADDR) &&
        (u32PageAddress < (DUCAR_CAN_IAP_APP1_START_ADDR + u32CanIapPackageSize)))
    {
        /* 绝对地址减APP1基址得到包内偏移。 */
        u32PageOffset = u32PageAddress - DUCAR_CAN_IAP_APP1_START_ADDR;
    }
    else
    {
        /* 非APP1地址范围按相对偏移解释。 */
        u32PageOffset = u32PageAddress;
    }
    /* 页偏移必须等于已收包长度、128字节对齐且不超包尾。 */
    if ((u32PageOffset != u32CanIapPackageReceived) ||
        ((u32PageOffset % DUCAR_CAN_IAP_PAGE_SIZE) != 0UL) ||
        (u32PageOffset >= u32CanIapPackageSize))
    {
        return false;
    }

    /* 将规范化后的页偏移返回给写入函数。 */
    *pu32PageOffset = u32PageOffset;
    return true;
}

/* 将已收齐的132字节页解析为地址/数据，并按首包或普通固件页写入APP2。 */
static bool ducar_can_iap_write_completed_page(void)
{
    uint32_t u32PageAddress;
    uint32_t u32PageOffset;
    uint32_t u32FirmwareBytesRemaining;
    uint32_t u32WriteLength;

    /* 页缓存前4字节是主机携带的页地址。 */
    u32PageAddress = ducar_can_iap_read_u32_le(au8CanIapPageBuffer);
    if (u8CanIapHeaderReceived == 0U)
    {
        /* 第一个页面地址只能是偏移0或APP1绝对地址。 */
        /* 首页只能从包偏移0或APP1绝对基址开始。 */
        if ((u32PageAddress != 0UL) &&
            (u32PageAddress != DUCAR_CAN_IAP_APP1_START_ADDR))
        {
            return false;
        }
        /* 首页头校验失败时，不擦除或写入任何APP2数据。 */
        if (!ducar_can_iap_parse_first_page())
        {
            return false;
        }
        /* 首页已完成接收，包进度推进128字节。 */
        u32CanIapPackageReceived += DUCAR_CAN_IAP_PAGE_SIZE;
        return true;
    }

    /* 普通页地址必须连续且不能落在升级头区域。 */
    if (!ducar_can_iap_page_address_is_expected(u32PageAddress, &u32PageOffset) ||
        (u32PageOffset < DUCAR_CAN_IAP_PACKAGE_HEADER_SIZE))
    {
        return false;
    }
    /* 从包偏移中去掉128字节升级头，得到固件在APP2中的写入偏移。 */
    u32PageOffset -= DUCAR_CAN_IAP_PACKAGE_HEADER_SIZE;
    if (u32PageOffset >= tCanIapFirmwareInfo.size)
    {
        return false;
    }

    /* 计算当前页之后还剩多少实际固件数据。 */
    u32FirmwareBytesRemaining = tCanIapFirmwareInfo.size - u32PageOffset;
    /* 最后一页不足128字节时只写剩余长度。 */
    u32WriteLength = (u32FirmwareBytesRemaining > DUCAR_CAN_IAP_PAGE_SIZE) ?
                     DUCAR_CAN_IAP_PAGE_SIZE : u32FirmwareBytesRemaining;
    /* 将当前页固件数据写入APP2对应偏移。 */
    if (!d_flash_write(DUCAR_CAN_IAP_APP2_START_ADDR + u32PageOffset,
                       &au8CanIapPageBuffer[DUCAR_CAN_IAP_PAGE_DATA_OFFSET],
                       u32WriteLength))
    {
        return false;
    }

    /* 累加实际固件字节数。 */
    u32CanIapFirmwareReceived += u32WriteLength;
    /* 包进度仍按完整128字节页推进。 */
    u32CanIapPackageReceived += DUCAR_CAN_IAP_PAGE_SIZE;
    return true;
}

/* 检查APP2向量表的栈地址和复位入口，避免CRC正确但入口地址非法。 */
static bool ducar_can_iap_app2_vector_is_valid(void)
{
    uint32_t u32StackAddress;
    uint32_t u32ResetAddress;

    /* 向量表第0项是初始栈地址。 */
    u32StackAddress = *((volatile const uint32_t *)DUCAR_CAN_IAP_APP2_START_ADDR);
    /* 向量表第1项是复位入口地址。 */
    u32ResetAddress = *((volatile const uint32_t *)(DUCAR_CAN_IAP_APP2_START_ADDR + 4UL));
    /* 栈必须落在片内SRAM，复位入口必须为Thumb地址且位于APP1。 */
    return ((u32StackAddress >= DUCAR_CAN_IAP_SRAM_START_ADDR) &&
            (u32StackAddress < DUCAR_CAN_IAP_SRAM_END_ADDR) &&
            ((u32ResetAddress & 1UL) != 0UL) &&
            ((u32ResetAddress & ~1UL) >= DUCAR_CAN_IAP_APP1_START_ADDR) &&
            ((u32ResetAddress & ~1UL) < DUCAR_CAN_IAP_APP1_END_ADDR));
}

/* 在最后一页写入后校验页数、固件字节数、向量表和头部CRC32。 */
static bool ducar_can_iap_complete_image_is_valid(void)
{
    uint32_t u32CalculatedCrc32;

    /* 必须已有合法头部。 */
    if ((u8CanIapHeaderReceived == 0U) ||
        /* 包接收长度必须等于头部计算出的总长度。 */
        (u32CanIapPackageReceived != u32CanIapPackageSize) ||
        /* 实际写入固件字节数必须等于头部声明长度。 */
        (u32CanIapFirmwareReceived != tCanIapFirmwareInfo.size) ||
        /* 向量表必须可启动。 */
        !ducar_can_iap_app2_vector_is_valid())
    {
        return false;
    }

    /* 对APP2实际固件区计算CRC32。 */
    u32CalculatedCrc32 = ducar_iap_crc32_calc(
        (const uint8_t *)DUCAR_CAN_IAP_APP2_START_ADDR,
        tCanIapFirmwareInfo.size);
    /* 只有计算值和首页头部CRC完全一致才认为整包有效。 */
    return (u32CalculatedCrc32 == tCanIapFirmwareInfo.crc32);
}

/* 完成当前页处理，发送EOM；只有整包校验通过才进入复位搬运挂起状态。 */
static void ducar_can_iap_complete_page(void)
{
    uint8_t u8MoveAfterEom;

    /* 先把当前132字节页写入APP2或完成首页头解析。 */
    if (!ducar_can_iap_write_completed_page())
    {
        /* 页写入失败时发送ABORT并清除会话。 */
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }

    /* 每页都回复EOM；只有当前已是完整合法镜像时才设置搬运标志。 */
    u8MoveAfterEom = ducar_can_iap_complete_image_is_valid() ? 1U : 0U;
    if (!ducar_can_iap_queue_eom(u8MoveAfterEom))
    {
        /* EOM无法排队时终止会话，避免主机误以为本页已确认。 */
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }

    /* 完整镜像有效时，等待EOM真正进入CAN邮箱后再准备BOOT搬运。 */
    if (u8MoveAfterEom != 0U)
    {
        /* EOM先进入发送状态；发送成功后才写CCCC并准备复位。 */
        eCanIapState = DUCAR_CAN_IAP_STATE_RESET_PENDING;
    }
    else
    {
        /* 当前页已确认，但全包尚未完成，继续等待下一页RTS。 */
        eCanIapState = DUCAR_CAN_IAP_STATE_WAIT_PAGE;
    }
}

/* 处理E1/01查询区域、E1/02开始升级、E1/03升级完成确认。 */
static void ducar_can_iap_receive_e1(const t_can_packet *ptCanPacket)
{
    uint8_t u8Command;
    uint32_t u32FirmwareSize;

    /* E1控制必须是8字节扩展帧、来自PC且当前没有待发应答。 */
    if ((ptCanPacket == 0) ||
        (ptCanPacket->u8len != DUCAR_CAN_IAP_DATA_DLC) ||
        !ducar_can_iap_is_pc_to_bms(ptCanPacket) ||
        (eCanIapTxState != DUCAR_CAN_IAP_TX_IDLE))
    {
        return;
    }

    /* Data0是E1子命令。 */
    u8Command = ptCanPacket->au8data[0];
    if (u8Command == 0x01U)
    {
        /* E1/01只查询APP当前所在区域。 */
        (void)ducar_can_iap_queue_e1_response(DUCAR_CAN_ACK,
                                               ducar_can_iap_get_sa(ptCanPacket));
        return;
    }

    /* E1/02开始新的升级接收会话。 */
    if (u8Command == 0x02U)
    {
        /* E1/02的Data1~Data4是整包固件长度，小端序；此处不复位、不写BBBB。 */
        /* Data1~Data4是主机声明的整个固件长度。 */
        u32FirmwareSize = ducar_can_iap_read_u32_le(&ptCanPacket->au8data[1]);
        if (!ducar_can_iap_firmware_size_is_valid(u32FirmwareSize))
        {
            (void)ducar_can_iap_queue_e1_response(DUCAR_CAN_NAK,
                                                   ducar_can_iap_get_sa(ptCanPacket));
            return;
        }

        /* 合法长度下清空旧会话，避免新升级沿用旧进度。 */
        ducar_can_iap_reset_transfer_state();
        /* 保存主机声明长度，首页头部还要再次核对。 */
        u32CanIapExpectedFirmwareSize = u32FirmwareSize;
        /* 记录主机源地址，后续CTS/EOM/ABORT都回给该地址。 */
        u8CanIapHostAddress = ducar_can_iap_get_sa(ptCanPacket);
        /* 从当前时间开始计算接收超时。 */
        u32CanIapLastActivityMs = d_timer_get_ms();
        /* E1/02成功后等待下一页RTS。 */
        eCanIapState = DUCAR_CAN_IAP_STATE_WAIT_PAGE;
        /* 回复APP区域ACK。 */
        (void)ducar_can_iap_queue_e1_response(DUCAR_CAN_ACK,
                                               u8CanIapHostAddress);
        return;
    }

    /* E1/03只在BOOT搬运完成、APP重新运行后查询。 */
    if (u8Command == 0x03U)
    {
        /* E1/03只能由APP在BOOT搬运成功、复位返回APP后响应。 */
        /* APP自身空闲且BOOT标志已经变为APP有效时才回复ACK。 */
        if ((eCanIapState == DUCAR_CAN_IAP_STATE_IDLE) &&
            (*((volatile const uint32_t *)APP_FLASH_MAP_BOOT_UPGRADE_FLAG_ADDR) ==
             DUCAR_CAN_IAP_UPGRADE_FLAG_APP_VALID))
        {
            (void)ducar_can_iap_queue_e1_response(DUCAR_CAN_ACK,
                                                   ducar_can_iap_get_sa(ptCanPacket));
        }
        /* 搬运中或搬运失败时不伪造E1/03成功应答，上位机需在APP恢复后重发。 */
        return;
    }

    /* 未知E1子命令统一回复NAK。 */
    (void)ducar_can_iap_queue_e1_response(DUCAR_CAN_NAK,
                                           ducar_can_iap_get_sa(ptCanPacket));
}

/* 处理主机针对DF00一页数据发起的RTS，建立本页接收上下文并回复CTS。 */
static void ducar_can_iap_receive_rts(const t_can_packet *ptCanPacket)
{
    uint16_t u16PageMessageLength;

    /* RTS必须来自PC、指向BMS、长度为8、控制字为RTS且PGN为DF00。 */
    if ((ptCanPacket == 0) ||
        !ducar_can_iap_is_pc_to_bms(ptCanPacket) ||
        (ptCanPacket->u8len != DUCAR_CAN_IAP_DATA_DLC) ||
        (ptCanPacket->au8data[0] != DUCAR_CAN_IAP_TP_RTS) ||
        !ducar_can_iap_is_df00_control(ptCanPacket))
    {
        return;
    }

    /* Data1~Data2为本页132字节总长度。 */
    u16PageMessageLength = ducar_can_iap_read_u16_le(&ptCanPacket->au8data[1]);
    /* 只有等待页、长度132、包数19同时满足时才接受RTS。 */
    if ((eCanIapState != DUCAR_CAN_IAP_STATE_WAIT_PAGE) ||
        (u16PageMessageLength != DUCAR_CAN_IAP_PAGE_MESSAGE_LEN) ||
        (ptCanPacket->au8data[3] != DUCAR_CAN_IAP_PAGE_PACKET_COUNT))
    {
        /* RTS异常时保存主机地址，便于发送ABORT。 */
        u8CanIapHostAddress = ducar_can_iap_get_sa(ptCanPacket);
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }

    /* 新页开始时清空132字节页缓存，最后一帧不足7字节部分保持FF。 */
    memset(au8CanIapPageBuffer, 0xFF, sizeof(au8CanIapPageBuffer));
    /* 保存本页主机地址、长度和DT总包数。 */
    u8CanIapHostAddress = ducar_can_iap_get_sa(ptCanPacket);
    u16CanIapPageLength = u16PageMessageLength;
    u8CanIapPagePackets = ptCanPacket->au8data[3];
    /* DT序号从1开始。 */
    u8CanIapExpectedPacket = 1U;
    /* RTS到达时刷新会话活动时间。 */
    u32CanIapLastActivityMs = d_timer_get_ms();
    /* 进入本页接收状态。 */
    eCanIapState = DUCAR_CAN_IAP_STATE_RECEIVING_PAGE;
    /* CTS无法排队时放弃本页会话。 */
    if (!ducar_can_iap_queue_cts())
    {
        ducar_can_iap_reset_transfer_state();
    }
}

/* 按序接收一页的DT数据；收齐19帧后进入页写入、EOM和整包校验流程。 */
static void ducar_can_iap_receive_dt(const t_can_packet *ptCanPacket)
{
    uint16_t u16PageBufferOffset;
    uint16_t u16PageBytesRemaining;
    uint8_t u8PageCopyLength;

    /* DT必须来自PC、至少有序号和1字节数据，并且当前正处于收页状态。 */
    if ((ptCanPacket == 0) ||
        !ducar_can_iap_is_pc_to_bms(ptCanPacket) ||
        (ptCanPacket->u8len < 2U) ||
        (eCanIapState != DUCAR_CAN_IAP_STATE_RECEIVING_PAGE))
    {
        return;
    }
    /* Data0必须等于当前期待的连续DT序号。 */
    if (ptCanPacket->au8data[0] != u8CanIapExpectedPacket)
    {
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }

    /* 每个DT从Data1开始携带7字节，按序号计算页内偏移。 */
    u16PageBufferOffset = (uint16_t)((uint16_t)(u8CanIapExpectedPacket - 1U) * 7U);
    /* 偏移不能越过当前页有效长度。 */
    if (u16PageBufferOffset >= u16CanIapPageLength)
    {
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }
    /* 计算当前序号之后还需接收多少页数据。 */
    u16PageBytesRemaining = (uint16_t)(u16CanIapPageLength - u16PageBufferOffset);
    /* 最后一帧可能少于7字节，按剩余长度拷贝。 */
    u8PageCopyLength = (u16PageBytesRemaining > 7U) ? 7U : (uint8_t)u16PageBytesRemaining;
    /* 实际DLC必须覆盖序号字节加需要拷贝的数据。 */
    if (ptCanPacket->u8len < (uint8_t)(u8PageCopyLength + 1U))
    {
        (void)ducar_can_iap_queue_abort();
        ducar_can_iap_reset_transfer_state();
        return;
    }

    /* 将DT Data1开始的有效字节复制进页缓存。 */
    memcpy(&au8CanIapPageBuffer[u16PageBufferOffset],
           &ptCanPacket->au8data[1],
           u8PageCopyLength);
    /* 每收到一帧DT都刷新升级会话活动时间。 */
    u32CanIapLastActivityMs = d_timer_get_ms();
    /* 收到最后一帧时立即完成本页处理。 */
    if (u8CanIapExpectedPacket >= u8CanIapPagePackets)
    {
        ducar_can_iap_complete_page();
    }
    else
    {
        /* 非最后一帧只推进期待序号，继续等待下一帧DT。 */
        u8CanIapExpectedPacket++;
    }
}

/* 初始化APP侧CAN IAP接收和发送状态机。 */
void ducar_can_iap_protocol_init(void)
{
    /* 初始化APP侧IAP接收状态：清除页接收、包计数和升级头信息。 */
    ducar_can_iap_reset_transfer_state();

    /* 初始化APP侧IAP发送状态：清除待发送报文和发送后的动作标志。 */
    ducar_can_iap_clear_tx_state();
}

/* 判断报文是否属于APP侧CAN IAP；普通CAN报文留给普通协议处理。 */
bool ducar_can_iap_protocol_is_packet(const t_can_packet *ptCanPacket)
{
    uint8_t u8Pf;

    /* 空指针、非扩展帧以及超过CAN最大数据长度的报文都不能进入IAP。 */
    if ((ptCanPacket == 0) ||
        (ptCanPacket->u32idType != D_CAN_ID_EXTENDED) ||
        (ptCanPacket->u8len > DUCAR_CAN_MAX_DATA_LEN))
    {
        return false;
    }

    /* 先从29位CAN ID中提取PF，后续根据协议功能码选择处理路径。 */
    u8Pf = ducar_can_iap_get_pf(ptCanPacket);

    /* PF=E1属于升级控制报文，APP在任意合法升级控制阶段都需要接收。 */
    if (u8Pf == DUCAR_CAN_PF_BOOT_CONTROL)
    {
        return true;
    }

    /* RTS只允许在等待新页时接收，并且必须来自PC、指向DF00数据传输目标。 */
    if ((u8Pf == DUCAR_CAN_PF_TP_CM) &&
        (eCanIapState == DUCAR_CAN_IAP_STATE_WAIT_PAGE) &&
        (ptCanPacket->au8data[0] == DUCAR_CAN_IAP_TP_RTS) &&
        ducar_can_iap_is_df00_control(ptCanPacket) &&
        ducar_can_iap_is_pc_to_bms(ptCanPacket))
    {
        return true;
    }

    /* DT只允许在当前页已经由RTS建立后接收，并且至少包含序号字节。 */
    return ((u8Pf == DUCAR_CAN_PF_TP_DT) &&
            (eCanIapState == DUCAR_CAN_IAP_STATE_RECEIVING_PAGE) &&
            (ptCanPacket->u8len >= 2U) &&
            ducar_can_iap_is_pc_to_bms(ptCanPacket));
}

/* APP层CAN IAP接收入口：按PF将E1、RTS和DT分发到对应处理函数。 */
void ducar_can_iap_protocol_receive(const t_can_packet *ptCanPacket)
{
    uint8_t u8Pf;

    /* 入口先做统一过滤，避免普通CAN业务报文误触发IAP状态机。 */
    if (!ducar_can_iap_protocol_is_packet(ptCanPacket))
    {
        return;
    }

    /* 合法报文通过PF分类，确保E1、RTS和DT分别由对应函数处理。 */
    u8Pf = ducar_can_iap_get_pf(ptCanPacket);
    if (u8Pf == DUCAR_CAN_PF_BOOT_CONTROL)
    {
        /* E1负责查询、声明固件长度以及通知APP完成升级。 */
        ducar_can_iap_receive_e1(ptCanPacket);
    }
    else if (u8Pf == DUCAR_CAN_PF_TP_CM)
    {
        /* TP.CM中的RTS负责建立一页数据的接收窗口。 */
        ducar_can_iap_receive_rts(ptCanPacket);
    }
    else if (u8Pf == DUCAR_CAN_PF_TP_DT)
    {
        /* TP.DT负责按序接收当前页的实际固件数据。 */
        ducar_can_iap_receive_dt(ptCanPacket);
    }
}

/* 周期推进IAP超时、应答发送、EOM发送完成确认以及进入BOOT前的复位。 */
void ducar_can_iap_protocol_task(void)
{
    uint32_t u32CurrentTimeMs;

    /* 使用系统毫秒时基，统一处理接收超时、发送重试和发送完成等待。 */
    u32CurrentTimeMs = d_timer_get_ms();

    /* 只有等待页或正在收页时才需要检查接收超时。 */
    if (((eCanIapState == DUCAR_CAN_IAP_STATE_WAIT_PAGE) ||
         (eCanIapState == DUCAR_CAN_IAP_STATE_RECEIVING_PAGE)) &&
        ((uint32_t)(u32CurrentTimeMs - u32CanIapLastActivityMs) >=
         DUCAR_CAN_IAP_TIMEOUT_MS))
    {
        if (eCanIapState == DUCAR_CAN_IAP_STATE_RECEIVING_PAGE)
        {
            /* 收页中超时，先通知HOST当前页异常终止。 */
            (void)ducar_can_iap_queue_abort();
        }

        /* 无论是否成功发出ABORT，都退出本次页传输状态。 */
        ducar_can_iap_reset_transfer_state();
    }

    /* TX_PENDING表示报文已经准备好，但还没有成功提交给CAN发送邮箱。 */
    if (eCanIapTxState == DUCAR_CAN_IAP_TX_PENDING)
    {
        if (d_can_send_iap(&can0,
                           tCanIapTxPacket.u32id,
                           D_CAN_ID_EXTENDED,
                           tCanIapTxPacket.au8data,
                           tCanIapTxPacket.u8len))
        {
            /* 报文成功提交后，检查是否需要在EOM发送后进入BOOT搬运。 */
            if (u8CanIapMoveAfterTx != 0U)
            {
                /* 只有EOM已提交到CAN邮箱后，才允许写CCCC。 */
                if (ducar_iap_prepare_boot_move(tCanIapFirmwareInfo.size,
                                                tCanIapFirmwareInfo.crc32))
                {
                    /* CCCC已经准备好，等待CAN邮箱真正空闲后再复位。 */
                    eCanIapTxState = DUCAR_CAN_IAP_TX_WAIT_IDLE;
                    u8CanIapTxRetryTicks = 0U;
                }
                else
                {
                    /* 搬运参数准备失败，清除发送和接收状态，禁止错误跳转BOOT。 */
                    ducar_can_iap_clear_tx_state();
                    ducar_can_iap_reset_transfer_state();
                }
            }
            else
            {
                /* 普通E1/CTS/ABORT应答已发送完成，释放当前待发送状态。 */
                ducar_can_iap_clear_tx_state();
            }
        }
        else if (++u8CanIapTxRetryTicks >= DUCAR_CAN_IAP_TX_RETRY_TICKS)
        {
            /* CAN邮箱长期不可用时，不设置CCCC，也不复位进BOOT。 */
            ducar_can_iap_clear_tx_state();
            ducar_can_iap_reset_transfer_state();
        }

        /* 本次只处理一个待发送报文，避免一个周期内重复推进发送状态机。 */
        return;
    }

    /* TX_WAIT_IDLE表示EOM已经提交，当前只等待CAN硬件把最后一帧发送完。 */
    if (eCanIapTxState == DUCAR_CAN_IAP_TX_WAIT_IDLE)
    {
        if (d_can_is_tx_idle(&can0))
        {
            /* 最后一帧已经离开发送邮箱，此时复位后由BOOT执行搬运。 */
            d_system_reset();
        }
        else if (++u8CanIapTxRetryTicks >= DUCAR_CAN_IAP_TX_RETRY_TICKS)
        {
            /* CCCC已经写入；邮箱状态异常时仍让BOOT接管搬运。 */
            d_system_reset();
        }
    }
}
