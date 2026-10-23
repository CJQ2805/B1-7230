/*
 * APP侧CAN IAP入口分流和E1控制应答。
 * 本文件负责APP运行期间的区域查询、开始升级握手和DF00会话归属标记；
 * APP2页接收/校验由ducar_can_iap_protocol_app.c负责，BOOT侧由独立的
 * boot_can_upgrade.c处理，三者共用同一套DUCAR CAN IAP报文格式。
 */
#include "Protocol/IAP/ducar_can_iap_protocol.h"

#include <string.h>

#include "Driver/d_system.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"

#define DUCAR_CAN_IAP_DATA_DLC              ((uint8_t)8U)
#define DUCAR_CAN_IAP_WAIT_TICKS_MAX        ((uint16_t)1000U)
#define DUCAR_CAN_IAP_ACK_WAIT_TICKS_MAX    ((uint8_t)20U)
#define DUCAR_CAN_IAP_TP_RTS                ((uint8_t)0x10U)

typedef enum
{
    DUCAR_CAN_IAP_REPLY_IDLE = 0U,
    DUCAR_CAN_IAP_REPLY_SEND,
    DUCAR_CAN_IAP_REPLY_WAIT_TX
} ducar_can_iap_reply_state_t;

static ducar_can_iap_reply_state_t eCanIapReplyState;
static uint8_t u8CanIapReplyStatus;
static uint8_t u8CanIapReplyArea;
static uint8_t u8CanIapReplyDestination;
static uint8_t u8CanIapReplyResetAfterTx;
static uint8_t u8CanIapReplyWaitTicks;
static uint8_t u8CanIapTpSessionActive;
static uint16_t u16CanIapTpWaitTicks;

/* 组装APP侧IAP应答使用的扩展帧ID。 */
static uint32_t ducar_can_iap_build_id(uint8_t u8Pf,
                                       uint8_t u8Ps,
                                       uint8_t u8Sa)
{
    return ((((uint32_t)DUCAR_CAN_IAP_CONTROL_PRIORITY & 0x07UL) << 26U) |
            ((uint32_t)u8Pf << 16U) |
            ((uint32_t)u8Ps << 8U) |
            (uint32_t)u8Sa) & DUCAR_CAN_EXT_ID_MASK;
}

/* 校验TP连接管理帧的目标PGN是否为升级数据DF00。 */
static uint8_t ducar_can_iap_is_df00_control(const t_can_packet *ptCanPacket)
{
    if ((ptCanPacket == 0) ||
        (ptCanPacket->u8len != DUCAR_CAN_IAP_DATA_DLC) ||
        (ptCanPacket->au8data[5] != 0x00U) ||
        (ptCanPacket->au8data[6] != (uint8_t)(DUCAR_CAN_IAP_PGN >> 8U)) ||
        (ptCanPacket->au8data[7] != 0x00U))
    {
        return 0U;
    }

    return 1U;
}

/* 只缓存一条APP侧IAP应答，避免接收分发路径直接占用CAN邮箱。 */
static bool ducar_can_iap_queue_reply(uint8_t u8Status,
                                      uint8_t u8Area,
                                      uint8_t u8Destination,
                                      uint8_t u8ResetAfterTx)
{
    if (eCanIapReplyState != DUCAR_CAN_IAP_REPLY_IDLE)
    {
        return false;
    }

    u8CanIapReplyStatus = u8Status;
    u8CanIapReplyArea = u8Area;
    u8CanIapReplyDestination = u8Destination;
    u8CanIapReplyResetAfterTx = u8ResetAfterTx;
    u8CanIapReplyWaitTicks = 0U;
    eCanIapReplyState = DUCAR_CAN_IAP_REPLY_SEND;
    return true;
}

/* 将缓存的E1 ACK/NAK发送到主机，并带上APP区域标识。 */
static bool ducar_can_iap_send_reply(void)
{
    uint8_t au8CanData[DUCAR_CAN_IAP_DATA_DLC];

    memset(au8CanData, 0xFF, sizeof(au8CanData));
    au8CanData[0] = u8CanIapReplyStatus;
    au8CanData[1] = u8CanIapReplyArea;
    au8CanData[5] = 0U;
    au8CanData[6] = DUCAR_CAN_PF_BOOT_CONTROL;
    au8CanData[7] = 0U;

    return d_can_send(&can0,
                      ducar_can_iap_build_id(DUCAR_CAN_PF_ACK,
                                             u8CanIapReplyDestination,
                                             DUCAR_CAN_BMS_A_ADDRESS),
                      D_CAN_ID_EXTENDED,
                      au8CanData,
                      DUCAR_CAN_IAP_DATA_DLC);
}

/* 处理PF=E1控制：查询区域、设置BOOT等待包标志及拒绝错误阶段命令。 */
static void ducar_can_iap_receive_boot_control(const t_can_packet *ptCanPacket,
                                               uint8_t u8SourceAddress,
                                               uint8_t u8DestinationAddress)
{
    uint8_t u8Command;

    if ((ptCanPacket->u8len != DUCAR_CAN_IAP_DATA_DLC) ||
        (u8SourceAddress != DUCAR_CAN_PC_ADDRESS) ||
        (u8DestinationAddress != DUCAR_CAN_BMS_A_ADDRESS) ||
        (eCanIapReplyState != DUCAR_CAN_IAP_REPLY_IDLE))
    {
        return;
    }

    u8Command = ptCanPacket->au8data[0];
    if (u8Command == 0x01U)
    {
        /* E1/01：PC询问升级入口所在区域；当前帧仍由APP回复。 */
        (void)ducar_can_iap_queue_reply(DUCAR_CAN_ACK,
                                        DUCAR_IAP_IN_APP,
                                        u8SourceAddress,
                                        0U);
    }
    else if (u8Command == 0x02U)
    {
        /* E1/02：只写BBBB等待包标志；不擦APP2，首页校验由BOOT完成。 */
        if (ducar_iap_prepare_boot_wait_package())
        {
            (void)ducar_can_iap_queue_reply(DUCAR_CAN_ACK,
                                            DUCAR_IAP_IN_APP,
                                            u8SourceAddress,
                                            1U);
        }
        else
        {
            (void)ducar_can_iap_queue_reply(DUCAR_CAN_NAK,
                                            DUCAR_IAP_IN_APP,
                                            u8SourceAddress,
                                            0U);
        }
    }
    else
    {
        /* E1/03完成升级只在BOOT收到完整镜像后有效，APP阶段不执行。 */
        (void)ducar_can_iap_queue_reply(DUCAR_CAN_NAK,
                                        DUCAR_IAP_IN_APP,
                                        u8SourceAddress,
                                        0U);
    }
}

/* 初始化APP侧IAP应答状态和DF00会话观察计时器。 */
void ducar_can_iap_protocol_init(void)
{
    eCanIapReplyState = DUCAR_CAN_IAP_REPLY_IDLE;
    u8CanIapReplyStatus = DUCAR_CAN_NAK;
    u8CanIapReplyArea = DUCAR_IAP_IN_APP;
    u8CanIapReplyDestination = DUCAR_CAN_PC_ADDRESS;
    u8CanIapReplyResetAfterTx = 0U;
    u8CanIapReplyWaitTicks = 0U;
    u8CanIapTpSessionActive = 0U;
    u16CanIapTpWaitTicks = 0U;
}

/* 判断报文是否属于APP侧IAP入口；符合条件的报文由app_can优先分流。 */
bool ducar_can_iap_protocol_is_packet(const t_can_packet *ptCanPacket)
{
    uint8_t u8Pf;

    if ((ptCanPacket == 0) ||
        (ptCanPacket->u32idType != D_CAN_ID_EXTENDED) ||
        (ptCanPacket->u8len > DUCAR_CAN_MAX_DATA_LEN))
    {
        return false;
    }

    u8Pf = (uint8_t)((ptCanPacket->u32id >> 16U) & 0xFFUL);
    if (u8Pf == DUCAR_CAN_PF_BOOT_CONTROL)
    {
        return true;
    }

    if ((u8Pf == DUCAR_CAN_PF_TP_CM) &&
        ducar_can_iap_is_df00_control(ptCanPacket) &&
        (ptCanPacket->au8data[0] == DUCAR_CAN_IAP_TP_RTS) &&
        ((uint8_t)(ptCanPacket->u32id & 0xFFUL) == DUCAR_CAN_PC_ADDRESS) &&
        ((uint8_t)((ptCanPacket->u32id >> 8U) & 0xFFUL) == DUCAR_CAN_BMS_A_ADDRESS))
    {
        return true;
    }

    return ((u8CanIapTpSessionActive != 0U) &&
            ((u8Pf == DUCAR_CAN_PF_TP_CM) ||
             (u8Pf == DUCAR_CAN_PF_TP_DT)) &&
            ((uint8_t)(ptCanPacket->u32id & 0xFFUL) == DUCAR_CAN_PC_ADDRESS) &&
            ((uint8_t)((ptCanPacket->u32id >> 8U) & 0xFFUL) == DUCAR_CAN_BMS_A_ADDRESS));
}

/* 分发APP侧IAP报文，并记录DF00会话活动以便处理超时。 */
void ducar_can_iap_protocol_receive(const t_can_packet *ptCanPacket)
{
    uint8_t u8Pf;
    uint8_t u8SourceAddress;
    uint8_t u8DestinationAddress;

    if (!ducar_can_iap_protocol_is_packet(ptCanPacket))
    {
        return;
    }

    u8Pf = (uint8_t)((ptCanPacket->u32id >> 16U) & 0xFFUL);
    u8DestinationAddress = (uint8_t)((ptCanPacket->u32id >> 8U) & 0xFFUL);
    u8SourceAddress = (uint8_t)(ptCanPacket->u32id & 0xFFUL);

    if (u8Pf == DUCAR_CAN_PF_BOOT_CONTROL)
    {
        ducar_can_iap_receive_boot_control(ptCanPacket,
                                          u8SourceAddress,
                                          u8DestinationAddress);
    }
    else if ((u8Pf == DUCAR_CAN_PF_TP_CM) &&
             ducar_can_iap_is_df00_control(ptCanPacket) &&
             (ptCanPacket->au8data[0] == DUCAR_CAN_IAP_TP_RTS))
    {
        /* DF00数据页由BOOT接收；APP只保持分类归属，不写APP2。 */
        u8CanIapTpSessionActive = 1U;
        u16CanIapTpWaitTicks = 0U;
    }
    else if ((u8Pf == DUCAR_CAN_PF_TP_DT) &&
             (u8CanIapTpSessionActive != 0U))
    {
        u16CanIapTpWaitTicks = 0U;
    }
    else
    {
        /* 当前APP侧CAN IAP不接收DF00页内容；BOOT负责页序、头、CRC和写Flash。 */
    }
}

/* 周期发送APP侧应答，并在E1/02 ACK发出后等待邮箱空闲再复位进BOOT。 */
void ducar_can_iap_protocol_task(void)
{
    if (u8CanIapTpSessionActive != 0U)
    {
        if (++u16CanIapTpWaitTicks >= DUCAR_CAN_IAP_WAIT_TICKS_MAX)
        {
            u8CanIapTpSessionActive = 0U;
            u16CanIapTpWaitTicks = 0U;
        }
    }

    if (eCanIapReplyState == DUCAR_CAN_IAP_REPLY_SEND)
    {
        if (ducar_can_iap_send_reply())
        {
            if (u8CanIapReplyResetAfterTx != 0U)
            {
                eCanIapReplyState = DUCAR_CAN_IAP_REPLY_WAIT_TX;
                u8CanIapReplyWaitTicks = 0U;
            }
            else
            {
                eCanIapReplyState = DUCAR_CAN_IAP_REPLY_IDLE;
            }
        }
        else if (++u8CanIapReplyWaitTicks >= DUCAR_CAN_IAP_ACK_WAIT_TICKS_MAX)
        {
            if (u8CanIapReplyResetAfterTx != 0U)
            {
                /* E1/02已写入BOOT等待包标志，邮箱无法接收ACK时也不能永久卡在APP。 */
                d_system_reset();
            }
            else
            {
                /* 普通区域查询/NAK应答无法排队时，放弃本次回复并释放状态。 */
                eCanIapReplyState = DUCAR_CAN_IAP_REPLY_IDLE;
                u8CanIapReplyWaitTicks = 0U;
            }
        }
        return;
    }

    if (eCanIapReplyState == DUCAR_CAN_IAP_REPLY_WAIT_TX)
    {
        if (d_can_is_tx_idle(&can0))
        {
            d_system_reset();
        }
        else if (++u8CanIapReplyWaitTicks >= DUCAR_CAN_IAP_ACK_WAIT_TICKS_MAX)
        {
            /* 邮箱异常时最多等100ms，再复位进入BOOT。 */
            d_system_reset();
        }
    }
}
