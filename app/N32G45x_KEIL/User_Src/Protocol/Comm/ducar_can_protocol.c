/*
 * 文件作用：DUCAR CAN普通协议实现。
 *
 * 本文件参考GD7627的组织方式：一个1ms基准计数器、明确的周期槽位、
 * 一个PGN对应一个组包发送函数。d_can只负责硬件和原始帧队列；
 * app_can负责协议入口分流；本文件负责普通DUCAR报文的组包、周期发送、
 * 请求应答、TP发送和普通控制命令处理。CAN IAP不在这里实现。
 */
#include "Protocol/Comm/ducar_can_protocol.h"

#include <string.h>

#include "App/app_bms_fault.h"
#include "App/app_balance_manage.h"
#include "App/app_current_calib.h"
#include "App/app_log_manage.h"
#include "App/app_h_power_manage.h"
#include "App/app_power_manage.h"
#include "App/app_hw_apply.h"
#include "App/app_bms_status.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "App/app_bms_snapshot.h"
#include "App/app_charge_time.h"
#include "Driver/d_rtc.h"
#include "Driver/d_system.h"
#include "Protocol/Comm/ducar_uart_protocol.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#include "Protocol/IAP/ducar_can_iap_protocol.h"
#include "Task/bms_task.h"

#define DUCAR_CAN_INVALID_U16                 ((uint16_t)0xFFFFU)
/* CAN温度分辨率为0.1℃；无效值按-40.0℃发送，即-400=0xFE70，小端字节为70 FE。 */
#define DUCAR_CAN_INVALID_S16                 ((int16_t)-400)
#define DUCAR_CAN_INVALID_S8                  ((uint8_t)0x80U)
#define DUCAR_CAN_DATA_DLC                    ((uint8_t)8U)

#define DUCAR_CAN_CYCLE_50MS                  ((uint16_t)50U)
#define DUCAR_CAN_CYCLE_100MS                 ((uint16_t)100U)
#define DUCAR_CAN_CYCLE_200MS                 ((uint16_t)200U)
#define DUCAR_CAN_CYCLE_500MS                 ((uint16_t)500U)
#define DUCAR_CAN_CYCLE_1000MS                ((uint16_t)1000U)

/* F220欠压点的低温切换滞回：-1.0℃进入，+1.0℃退出。 */
#define DUCAR_CAN_F220_LOW_TEMP_ENTER_0P1C    ((int16_t)-10)
#define DUCAR_CAN_F220_LOW_TEMP_EXIT_0P1C     ((int16_t)10)

#define DUCAR_CAN_DETD_TX_WINDOW_TICKS        ((uint16_t)5000U)
#define DUCAR_CAN_TP_MAX_DATA_LEN             ((uint8_t)60U)
#define DUCAR_CAN_ACK_WAIT_MAX_TICKS          ((uint8_t)100U)
#define DUCAR_CAN_TP_RTS                      ((uint8_t)0x10U)
#define DUCAR_CAN_TP_CTS                      ((uint8_t)0x11U)
#define DUCAR_CAN_TP_EOM_ACK                  ((uint8_t)0x13U)
#define DUCAR_CAN_TP_ABORT                    ((uint8_t)0xFFU)

/* app_bms_fault_image_t的原始位图，位序与app_bms_fault.h保持一致。 */
#define DUCAR_CAN_CHG_FAULT_A_COV_WARN         ((uint16_t)1U << 0U)
#define DUCAR_CAN_CHG_FAULT_A_COV_PRO          ((uint16_t)1U << 1U)
#define DUCAR_CAN_CHG_FAULT_A_OT_WARN          ((uint16_t)1U << 2U)
#define DUCAR_CAN_CHG_FAULT_A_OT_PRO           ((uint16_t)1U << 3U)
#define DUCAR_CAN_CHG_FAULT_A_UT_WARN          ((uint16_t)1U << 4U)
#define DUCAR_CAN_CHG_FAULT_A_UT_PRO           ((uint16_t)1U << 5U)
#define DUCAR_CAN_CHG_FAULT_A_OC_WARN          ((uint16_t)1U << 6U)
#define DUCAR_CAN_CHG_FAULT_A_OC_PRO1          ((uint16_t)1U << 7U)
#define DUCAR_CAN_CHG_FAULT_A_COV_PRO2         ((uint16_t)1U << 8U)
#define DUCAR_CAN_CHG_FAULT_A_RECHARGE_OCC     ((uint16_t)1U << 9U)
#define DUCAR_CAN_CHG_FAULT_A_OC_PRO2          ((uint16_t)1U << 11U)
#define DUCAR_CAN_CHG_FAULT_A_RECHARGE_WARN    ((uint16_t)1U << 12U)
#define DUCAR_CAN_CHG_FAULT_B_PACK_OV_WARN     ((uint16_t)1U << 0U)
#define DUCAR_CAN_CHG_FAULT_B_PACK_OV_PRO      ((uint16_t)1U << 1U)

#define DUCAR_CAN_DSG_FAULT_A_CUV_WARN         ((uint16_t)1U << 0U)
#define DUCAR_CAN_DSG_FAULT_A_CUV_PRO1         ((uint16_t)1U << 1U)
#define DUCAR_CAN_DSG_FAULT_A_CUV_DEEP         ((uint16_t)1U << 2U)
#define DUCAR_CAN_DSG_FAULT_A_OT_WARN          ((uint16_t)1U << 3U)
#define DUCAR_CAN_DSG_FAULT_A_OT_PRO           ((uint16_t)1U << 4U)
#define DUCAR_CAN_DSG_FAULT_A_UT_WARN          ((uint16_t)1U << 5U)
#define DUCAR_CAN_DSG_FAULT_A_UT_PRO           ((uint16_t)1U << 6U)
#define DUCAR_CAN_DSG_FAULT_A_OC_WARN          ((uint16_t)1U << 7U)
#define DUCAR_CAN_DSG_FAULT_A_OC_LOCK          ((uint16_t)1U << 11U)
#define DUCAR_CAN_DSG_FAULT_A_LOW_SOC          ((uint16_t)1U << 12U)
#define DUCAR_CAN_DSG_FAULT_A_CUV_PRO2         ((uint16_t)1U << 13U)
#define DUCAR_CAN_DSG_FAULT_B_OC_PRO1          ((uint16_t)1U << 0U)
#define DUCAR_CAN_DSG_FAULT_B_PACK_UV_WARN     ((uint16_t)1U << 1U)
#define DUCAR_CAN_DSG_FAULT_B_PACK_UV_PRO      ((uint16_t)1U << 2U)
#define DUCAR_CAN_DSG_FAULT_B_OC_PRO2          ((uint16_t)1U << 3U)
#define DUCAR_CAN_DSG_FAULT_B_OC_PRO3          ((uint16_t)1U << 4U)
#define DUCAR_CAN_DSG_FAULT_B_OC_PRO4          ((uint16_t)1U << 5U)

#define DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_WARN ((uint16_t)1U << 1U)
#define DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_PRO  ((uint16_t)1U << 2U)
#define DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_PRO2 ((uint16_t)1U << 12U)
#define DUCAR_CAN_GENERAL_FAULT_CMOS_FAIL      ((uint16_t)1U << 3U)
#define DUCAR_CAN_GENERAL_FAULT_DMOS_FAIL      ((uint16_t)1U << 4U)
#define DUCAR_CAN_GENERAL_FAULT_MOS_OT_PRO     ((uint16_t)1U << 5U)
#define DUCAR_CAN_GENERAL_FAULT_MOS_OT_WARN    ((uint16_t)1U << 8U)

#define DUCAR_CAN_F21F_CHG_MOS_FAIL_PROTECT    ((uint8_t)1U << 0U)
#define DUCAR_CAN_F21F_DSG_MOS_FAIL_PROTECT    ((uint8_t)1U << 1U)

static uint32_t ducar_can_id_encode(uint8_t u8Pf, uint8_t u8Ps, uint8_t u8Sa);

/* 本工程只保留一个1ms节拍计数器，所有周期槽都由它派生。 */
static uint16_t u16Can1msCount;
static uint16_t u16DetdProactiveTxTicks;
static uint16_t u16CanLifeValueTicks;
static uint8_t u8CanLifeValue;
static uint8_t u8DetdConnectedLast;
static uint8_t u8CanBusSilent;
static uint8_t u8CanF220LowTempUvActive;
static uint8_t u8CanIapSilenceActive;

typedef enum
{
    DUCAR_CAN_ACTION_NONE = 0U,
    DUCAR_CAN_ACTION_RESET,
    DUCAR_CAN_ACTION_POWER_OFF
} ducar_can_action_t;

typedef enum
{
    DUCAR_CAN_TP_IDLE = 0U,
    DUCAR_CAN_TP_SEND_RTS,
    DUCAR_CAN_TP_WAIT_CTS,
    DUCAR_CAN_TP_SEND_DT,
    DUCAR_CAN_TP_WAIT_EOM
} ducar_can_tp_state_t;

static uint8_t u8CanAckPending;
static uint8_t u8CanAckStatus;
static uint8_t u8CanAckDestination;
static uint16_t u16CanAckFunctionCode;
static ducar_can_action_t eCanActionAfterAck;
static ducar_can_action_t eCanActionWaitingTx;
static uint8_t u8CanActionWaitTicks;

static ducar_can_tp_state_t eCanTpState;
static uint8_t au8CanTpData[DUCAR_CAN_TP_MAX_DATA_LEN];
static uint8_t u8CanTpDataLength;
static uint8_t u8CanTpPacketCount;
static uint8_t u8CanTpNextPacket;
static uint8_t u8CanTpBurstPackets;
static uint8_t u8CanTpDestination;
static uint16_t u16CanTpWaitTicks;
static uint16_t u16CanTpPgn;
static uint16_t u16CanLogRequested;
static uint16_t u16CanLogNext;
static uint8_t u8CanSocOcvAckPending;
static uint8_t u8CanSocOcvAckDestination;
static uint16_t u16CanSocOcvWaitTicks;
static uint8_t u8CanSocWriteAckPending;
static uint8_t u8CanSocWriteAckDestination;

typedef struct
{
    uint8_t data[8];
    uint8_t destination;
    uint32_t queued_ms;
} ducar_can_cal2_reply_t;

#define DUCAR_CAL2_REPLY_CAPACITY 4U
static ducar_can_cal2_reply_t s_cal2_replies[DUCAR_CAL2_REPLY_CAPACITY];
static uint8_t s_cal2_reply_head;
static uint8_t s_cal2_reply_count;
static uint32_t s_cal2_can_ms;

static bool ducar_can_cal2_queue_reply(uint8_t destination, uint8_t status,
                                      uint8_t operation, uint8_t sequence,
                                      uint16_t session)
{
    ducar_can_cal2_reply_t *reply;
    uint8_t index;
    if (s_cal2_reply_count >= DUCAR_CAL2_REPLY_CAPACITY) return false;
    index = (uint8_t)((s_cal2_reply_head + s_cal2_reply_count) % DUCAR_CAL2_REPLY_CAPACITY);
    reply = &s_cal2_replies[index];
    reply->data[0] = status;
    reply->data[1] = operation;
    reply->data[2] = sequence;
    reply->data[3] = (uint8_t)session;
    reply->data[4] = (uint8_t)(session >> 8U);
    reply->data[5] = 0U;
    reply->data[6] = 0xE7U;
    reply->data[7] = 0U;
    reply->destination = destination;
    reply->queued_ms = s_cal2_can_ms;
    ++s_cal2_reply_count;
    return true;
}

bool ducar_can_cal2_allows_sleep(void)
{
    return !app_current_calib_cal2_is_active() &&
           !app_current_calib_cal2_has_completion() &&
           (s_cal2_reply_count == 0U);
}

void ducar_can_get_tx_queue_diagnostics(ducar_can_tx_queue_diag_t *diagnostics)
{
    if (diagnostics == 0)
    {
        return;
    }

    diagnostics->cal2_reply_count = s_cal2_reply_count;
    diagnostics->ack_pending = u8CanAckPending;
    diagnostics->tp_state = (uint8_t)eCanTpState;
    diagnostics->tp_next_packet = u8CanTpNextPacket;
    diagnostics->tp_packet_count = u8CanTpPacketCount;
}

static void ducar_can_cal2_reply_task(void)
{
    app_current_calib_cal2_completion_t completion;
    ducar_can_cal2_reply_t *reply;
    if ((s_cal2_reply_count < DUCAR_CAL2_REPLY_CAPACITY) &&
        app_current_calib_cal2_take_completion(&completion))
    {
        (void)ducar_can_cal2_queue_reply(completion.destination, completion.status,
                                         completion.operation, completion.sequence,
                                         completion.session);
    }
    if (s_cal2_reply_count == 0U) return;
    reply = &s_cal2_replies[s_cal2_reply_head];
    if (((uint32_t)(s_cal2_can_ms - reply->queued_ms) >= 5000UL) ||
        d_can_send(&can0,
                   ducar_can_id_encode(DUCAR_CAN_PF_ACK, reply->destination,
                                       DUCAR_CAN_BMS_A_ADDRESS),
                   D_CAN_ID_EXTENDED, reply->data, 8U))
    {
        s_cal2_reply_head = (uint8_t)((s_cal2_reply_head + 1U) % DUCAR_CAL2_REPLY_CAPACITY);
        --s_cal2_reply_count;
    }
}

static bool ducar_can_tp_start(uint8_t destination, uint16_t pgn,
                               const uint8_t *data, uint8_t length);
static void ducar_can_log_next(void)
{
    uint8_t data[60];
    if ((u16CanLogNext == 0U) || (u16CanLogNext > u16CanLogRequested))
    {
        u16CanLogNext = 0U;
        return;
    }
    if (!ducar_bms_pack_log_payload(u16CanLogNext, data) ||
        !ducar_can_tp_start(DUCAR_CAN_PC_ADDRESS, DUCAR_CAN_PGN_F232,
                            data, sizeof(data)))
    {
        u16CanLogNext = 0U;
    }
}
/* PC写入F230时的独立TP接收缓冲，与BMS主动发送TP互不复用。 */
static uint8_t au8CanTpRxSn[APP_BMS_MANUFACTURE_SN_MAX_LEN];
static uint8_t u8CanTpRxLength;
static uint8_t u8CanTpRxPackets;
static uint8_t u8CanTpRxNext;
static uint16_t u16CanTpRxWaitTicks;

/* 按DUCAR 29位扩展ID格式组合优先级、PF、PS和源地址。 */
static uint32_t ducar_can_id_encode(uint8_t u8Pf,
                                    uint8_t u8Ps,
                                    uint8_t u8Sa)
{
    /* 优先级固定为6，对应DUCAR普通CAN使用的18xxxxxx ID段。 */
    return ((((uint32_t)DUCAR_CAN_DEFAULT_PRIORITY & 0x07UL) << 26U) |
            /* 保留位和数据页位按A0.1固定为0，PF放在16~23位。 */
            ((uint32_t)u8Pf << 16U) |
            /* PS放在8~15位，表示目标地址。 */
            ((uint32_t)u8Ps << 8U) |
            /* SA放在0~7位，表示源地址。 */
            (uint32_t)u8Sa) & DUCAR_CAN_EXT_ID_MASK;
}

static void ducar_can_id_decode(uint32_t u32CanId,
                                ducar_can_id_fields_t *ptCanIdFields)
{
    if (ptCanIdFields == 0)
    {
        return;
    }

    /* 只保留扩展CAN ID的29个有效位，屏蔽上层可能带入的无效位。 */
    u32CanId &= DUCAR_CAN_EXT_ID_MASK;
    /* 提取优先级字段。 */
    ptCanIdFields->u8Priority = (uint8_t)((u32CanId >> 26U) & 0x07UL);
    /* 提取保留位字段。 */
    ptCanIdFields->u8Reserved = (uint8_t)((u32CanId >> 25U) & 0x01UL);
    /* 提取数据页字段。 */
    ptCanIdFields->u8DataPage = (uint8_t)((u32CanId >> 24U) & 0x01UL);
    /* 提取PF字段，用于一级协议分发。 */
    ptCanIdFields->u8Pf = (uint8_t)((u32CanId >> 16U) & 0xFFUL);
    /* 提取PS字段，用于判断报文目标是否为本BMS。 */
    ptCanIdFields->u8Ps = (uint8_t)((u32CanId >> 8U) & 0xFFUL);
    /* 提取SA字段，用于判断报文是否来自上位机。 */
    ptCanIdFields->u8Sa = (uint8_t)(u32CanId & 0xFFUL);
}

/* 只允许来自PC源地址的请求进入普通协议控制和查询分支。 */
bool ducar_can_id_is_pc_source(const ducar_can_id_fields_t *ptCanIdFields)
{
    return ((ptCanIdFields != 0) &&
            (ptCanIdFields->u8Sa == DUCAR_CAN_PC_ADDRESS));
}

static uint32_t ducar_can_build_bms_data_id(uint16_t u16Pgn)
{
    /* 普通BMS_A主动/查询回复统一使用源地址0x16。 */
    /* PGN高字节在PF位置，PGN低字节在PS位置。 */
    return ducar_can_id_encode((uint8_t)(u16Pgn >> 8U),
                               (uint8_t)u16Pgn,
                               DUCAR_CAN_BMS_A_ADDRESS);
}

static void ducar_can_write_u16_le(uint8_t *pau8CanData,
                                   uint8_t u8ByteOffset,
                                   uint16_t u16Value)
{
    /* A0.1规定多字节数值按小端格式写入CAN数据区。 */
    /* 低字节放在字段起始位置。 */
    pau8CanData[u8ByteOffset] = (uint8_t)(u16Value & 0xFFU);
    /* 高字节放在字段起始位置的下一字节。 */
    pau8CanData[(uint8_t)(u8ByteOffset + 1U)] =
        (uint8_t)((u16Value >> 8U) & 0xFFU);
}

static void ducar_can_write_s16_le(uint8_t *pau8CanData,
                                   uint8_t u8ByteOffset,
                                   int16_t s16Value)
{
    /* 有符号16位量沿用同一小端字节序，仅保留补码表示。 */
    /* 强制转换只改变解释方式，不改变底层16位补码。 */
    ducar_can_write_u16_le(pau8CanData, u8ByteOffset, (uint16_t)s16Value);
}

static uint16_t ducar_can_limit_u32_to_u16(uint32_t u32Value)
{
    /* 防止业务值超出协议U16字段后发生截断回绕。 */
    /* 超过协议上限时发送最大可表示值。 */
    return (u32Value > (uint32_t)UINT16_MAX) ? UINT16_MAX : (uint16_t)u32Value;
}

static uint16_t ducar_can_limit_s32_to_u16(int32_t s32Value)
{
    /* 无符号协议量遇到负值时按0发送，正值再限制到U16范围。 */
    /* 负数在无符号协议中没有语义，统一钳位为0。 */
    if (s32Value <= 0)
    {
        return 0U;
    }

    return ducar_can_limit_u32_to_u16((uint32_t)s32Value);
}

static int16_t ducar_can_limit_s32_to_s16(int32_t s32Value)
{
    /* 有符号协议量超范围时饱和到S16上下限。 */
    /* 先处理正向溢出。 */
    if (s32Value > (int32_t)INT16_MAX)
    {
        return INT16_MAX;
    }

    /* 再处理负向溢出。 */
    if (s32Value < (int32_t)INT16_MIN)
    {
        return INT16_MIN;
    }

    /* 在范围内时直接转换为协议有符号16位数值。 */
    return (int16_t)s32Value;
}

static uint16_t ducar_can_cell_voltage_mv(const app_bms_snapshot_t *ptBmsSnapshot,
                                          uint8_t u8CellIndex)
{
    /* 电芯不存在、快照未就绪或电压为0时，按协议无效值0xFFFF发送。 */
    /* 先判断快照指针，避免读取空指针。 */
    if ((ptBmsSnapshot == 0) ||
        /* data_ready为0表示快照还没有可发送的有效采样数据。 */
        (ptBmsSnapshot->data_ready == 0U) ||
        /* 电芯下标不能超过当前实际串数。 */
        (u8CellIndex >= ptBmsSnapshot->cell_count) ||
        /* 同时受当前配置的逻辑数组长度限制。 */
        (u8CellIndex >= APP_BMS_CELL_COUNT) ||
        /* 电压为0通常表示该通道无效，按协议填FFFF。 */
        (ptBmsSnapshot->cell_mv[u8CellIndex] == 0U))
    {
        return DUCAR_CAN_INVALID_U16;
    }

    /* 返回快照中的毫伏值，调用方再按协议字段写入。 */
    return ptBmsSnapshot->cell_mv[u8CellIndex];
}

static uint8_t ducar_can_cell_temperature_fault_bit(uint8_t u8TemperatureIndex)
{
    /* 快照温度索引0/1/2分别对应AFE_NTC、CELL_NTC1、CELL_NTC2。 */
    switch (u8TemperatureIndex)
    {
        case 0U:
            return APP_BMS_NTC_FAULT_AFE;
        case 1U:
            return APP_BMS_NTC_FAULT_CELL1;
        case 2U:
            return APP_BMS_NTC_FAULT_CELL2;
        default:
            return 0U;
    }
}

static int16_t ducar_can_cell_temperature_0p1c(const app_bms_snapshot_t *ptBmsSnapshot,
                                                uint8_t u8TemperatureIndex)
{
    uint8_t u8NtcFaultBit;

    /* 温度通道必须在快照有效位图中，否则返回协议无效温度。 */
    /* 先判断快照是否存在。 */
    if ((ptBmsSnapshot == 0) ||
        /* 温度索引必须落在编译期支持的通道范围内。 */
        (u8TemperatureIndex >= APP_BMS_MAX_TEMP_NUM))
    {
        return DUCAR_CAN_INVALID_S16;
    }

    u8NtcFaultBit = ducar_can_cell_temperature_fault_bit(u8TemperatureIndex);
    /* 显式NTC故障优先于采样有效位，故障通道统一按-40℃对外发送。 */
    if (((u8NtcFaultBit != 0U) &&
         ((ptBmsSnapshot->ntc.fault_mask & u8NtcFaultBit) != 0U)) ||
        /* valid_mask对应位为0时，不能把未采样值当成真实温度。 */
        ((ptBmsSnapshot->temp_valid_mask & (uint8_t)(1U << u8TemperatureIndex)) == 0U))
    {
        return DUCAR_CAN_INVALID_S16;
    }

    /* 返回以0.1℃为单位的有符号温度值。 */
    return ptBmsSnapshot->temp_0p1c[u8TemperatureIndex];
}

/* F20D：总压、电流、SOC、SOH和DETC状态。 */
static void ducar_can_send_f20d(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    uint32_t u32BatVoltage01V;
    int32_t s32Current01A;

    /* 从总快照读取本次报文需要的业务数据，避免直接访问各业务模块内部变量。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* 快照总压单位为mV，协议F20D要求0.01V，因此除以10。 */
    u32BatVoltage01V = tBmsSnapshot.bat_mv / 10U;
    /* 快照电流单位为mA，协议F20D要求0.01A，因此除以10并保留正负号。 */
    s32Current01A = tBmsSnapshot.current_business_ma / 10;

    /* F20D: 总压0.01V、实际电流0.01A、SOC0.1%、SOH、DETC接入状态。 */
    /* Data0~Data1写入总压。 */
    ducar_can_write_u16_le(au8CanData, 0U,
                            ducar_can_limit_u32_to_u16(u32BatVoltage01V));
    /* Data2~Data3写入实际电流。 */
    ducar_can_write_s16_le(au8CanData, 2U,
                            ducar_can_limit_s32_to_s16(s32Current01A));
    /* Data4~Data5写入显示SOC，显示百分比扩大10倍变成0.1%单位。 */
    ducar_can_write_u16_le(au8CanData, 4U,
                            (uint16_t)tBmsSnapshot.soc.u8display_soc * 10U);
    /* Data6写入SOH，直接沿用快照的百分比数值。 */
    au8CanData[6] = tBmsSnapshot.soc.u8display_soh;
    /* Data7使用1表示DETC在位，2表示DETC不在位。 */
    au8CanData[7] = (tBmsSnapshot.signal.input.mcu_detc != 0U) ? 1U : 2U;

    /* 使用F20D对应的BMS源地址发送8字节扩展帧。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F20D),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F20E：最大充放电电流以及最高、最低单体电压。 */
static void ducar_can_send_f20e(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取快照中的SOP和单体极值。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* F20E: 最大放电/充电电流0.01A，最高/最低单体电压mV。 */
    /* SOP源gt_app_sop使用0.1A；快照字段名带10mA但仍镜像0.1A，乘10转0.01A。 */
    ducar_can_write_u16_le(au8CanData, 0U,
                            ducar_can_limit_s32_to_u16(
                                (int32_t)tBmsSnapshot.sop.s16MaxDischargeCurrent10mA * 10));
    /* 最大允许充电电流写入Data2~Data3。 */
    ducar_can_write_u16_le(au8CanData, 2U,
                            ducar_can_limit_s32_to_u16(
                                (int32_t)tBmsSnapshot.sop.s16MaxChargeCurrent10mA * 10));
    /* 快照有效时写最高单体电压，否则写协议无效值FFFF。 */
    ducar_can_write_u16_le(au8CanData, 4U,
                            (tBmsSnapshot.data_ready != 0U) ?
                            tBmsSnapshot.max_cell_mv : DUCAR_CAN_INVALID_U16);
    /* 快照有效时写最低单体电压，否则写协议无效值FFFF。 */
    ducar_can_write_u16_le(au8CanData, 6U,
                            (tBmsSnapshot.data_ready != 0U) ?
                            tBmsSnapshot.min_cell_mv : DUCAR_CAN_INVALID_U16);

    /* 发送F20E单帧。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F20E),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F20F：发送单体1~4电压。 */
static void ducar_can_send_f20f(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取最新电芯电压快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* Data0~Data1对应第1串。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 0U));
    /* Data2~Data3对应第2串。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 1U));
    /* Data4~Data5对应第3串。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 2U));
    /* Data6~Data7对应第4串。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 3U));

    /* 发送F20F。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F20F),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F210：发送单体5~8电压。 */
static void ducar_can_send_f210(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取最新电芯电压快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* Data0~Data1对应第5串。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 4U));
    /* Data2~Data3对应第6串。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 5U));
    /* Data4~Data5对应第7串。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 6U));
    /* Data6~Data7对应第8串。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 7U));

    /* 发送F210。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F210),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F211：发送单体9~12电压。 */
static void ducar_can_send_f211(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取最新电芯电压快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* Data0~Data1对应第9串。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 8U));
    /* Data2~Data3对应第10串。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 9U));
    /* Data4~Data5对应第11串。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 10U));
    /* Data6~Data7对应第12串。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 11U));

    /* 发送F211。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F211),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F212：发送单体13~16电压。 */
static void ducar_can_send_f212(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取最新电芯电压快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* Data0~Data1对应第13串。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 12U));
    /* Data2~Data3对应第14串。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 13U));
    /* Data4~Data5对应第15串。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 14U));
    /* Data6~Data7对应第16串。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 15U));

    /* 发送F212。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F212),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F213：发送单体17~20电压。 */
static void ducar_can_send_f213(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取最新电芯电压快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* F213承载单体17~20，数据有效性由快照串数统一判断。 */
    /* Data0~Data1对应第17串。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 16U));
    /* Data2~Data3对应第18串。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 17U));
    /* Data4~Data5对应第19串。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 18U));
    /* Data6~Data7对应第20串。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 19U));

    /* 发送F213。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F213),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F214：发送单体21~24电压；20串项目无效通道填无效值。 */
static void ducar_can_send_f214(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取快照，即使当前是20串，F214仍按协议保留发送。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* 当前20串产品没有21~24串采样源，按A0.1无效值填充。 */
    /* Data0~Data1对应第21串，不存在时返回FFFF。 */
    ducar_can_write_u16_le(au8CanData, 0U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 20U));
    /* Data2~Data3对应第22串，不存在时返回FFFF。 */
    ducar_can_write_u16_le(au8CanData, 2U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 21U));
    /* Data4~Data5对应第23串，不存在时返回FFFF。 */
    ducar_can_write_u16_le(au8CanData, 4U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 22U));
    /* Data6~Data7对应第24串，不存在时返回FFFF。 */
    ducar_can_write_u16_le(au8CanData, 6U, ducar_can_cell_voltage_mv(&tBmsSnapshot, 23U));

    /* 发送F214。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F214),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F215：发送4路电芯温度。 */
static void ducar_can_send_f215(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取温度快照及各通道有效标志。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* F215: 4路电芯温度，单位0.1℃；第4路当前没有传感器，填无效值。 */
    /* Data0~Data1发送温度通道0。 */
    ducar_can_write_s16_le(au8CanData, 0U,
                            ducar_can_cell_temperature_0p1c(&tBmsSnapshot, 0U));
    /* Data2~Data3发送温度通道1。 */
    ducar_can_write_s16_le(au8CanData, 2U,
                            ducar_can_cell_temperature_0p1c(&tBmsSnapshot, 1U));
    /* Data4~Data5发送温度通道2。 */
    ducar_can_write_s16_le(au8CanData, 4U,
                            ducar_can_cell_temperature_0p1c(&tBmsSnapshot, 2U));
    /* Data6~Data7发送温度通道3，无效时由辅助函数返回-40.0℃（小端70 FE）。 */
    ducar_can_write_s16_le(au8CanData, 6U,
                            ducar_can_cell_temperature_0p1c(&tBmsSnapshot, 3U));

    /* 发送F215。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F215),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* 将0.1℃温度转为A0.5要求的整数摄氏度S8，并限制到有效编码范围。 */
static uint8_t ducar_can_temperature_s8(int16_t s16Temperature0p1C)
{
    int16_t s16TemperatureC = (int16_t)(s16Temperature0p1C / 10);

    if (s16TemperatureC > 127)
    {
        s16TemperatureC = 127;
    }
    else if (s16TemperatureC < -127)
    {
        s16TemperatureC = -127;
    }

    return (uint8_t)(int8_t)s16TemperatureC;
}

/* F216：主MOS/预放MOS温度使用S8整数℃；电芯温度仍为S16的0.1℃。 */
static void ducar_can_send_f216(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    int16_t s16MaxCellTemperature0p1C;
    int16_t s16MinCellTemperature0p1C;
    int16_t s16Temperature0p1C;
    uint8_t u8TemperatureIndex;
    uint8_t u8NtcFaultBit;
    uint8_t u8ValidTemperatureCount = 0U;

    /* 读取MOS温度、电芯极值温度和当前串数。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* 主MOS有有效采样时从0.1℃转换为整数℃；无效时按S8 -128发送。 */
    au8CanData[0] =
        ((tBmsSnapshot.ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U) ?
        ducar_can_temperature_s8(tBmsSnapshot.ntc.mos_temp_0p1c) : DUCAR_CAN_INVALID_S8;
    /* 预放电阻温度由PA2的PTC采样提供；仅用于通信显示，不参与温度保护。 */
    au8CanData[1] =
        ((tBmsSnapshot.ntc.valid_mask & APP_BMS_NTC_VALID_PTC) != 0U) ?
        ducar_can_temperature_s8(tBmsSnapshot.ntc.ptc_temp_0p1c) : DUCAR_CAN_INVALID_S8;

    /* 最高/最低温以及有效路数只统计有效且未报故障的AFE_NTC、NTC1和NTC2。 */
    s16MaxCellTemperature0p1C = DUCAR_CAN_INVALID_S16;
    s16MinCellTemperature0p1C = DUCAR_CAN_INVALID_S16;
    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < APP_BMS_MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        if ((tBmsSnapshot.temp_valid_mask & (uint8_t)(1U << u8TemperatureIndex)) == 0U)
        {
            continue;
        }

        u8NtcFaultBit = ducar_can_cell_temperature_fault_bit(u8TemperatureIndex);
        if ((u8NtcFaultBit != 0U) &&
            ((tBmsSnapshot.ntc.fault_mask & u8NtcFaultBit) != 0U))
        {
            continue;
        }

        s16Temperature0p1C = tBmsSnapshot.temp_0p1c[u8TemperatureIndex];
        if (u8ValidTemperatureCount == 0U)
        {
            s16MaxCellTemperature0p1C = s16Temperature0p1C;
            s16MinCellTemperature0p1C = s16Temperature0p1C;
        }
        else
        {
            if (s16Temperature0p1C > s16MaxCellTemperature0p1C)
            {
                s16MaxCellTemperature0p1C = s16Temperature0p1C;
            }
            if (s16Temperature0p1C < s16MinCellTemperature0p1C)
            {
                s16MinCellTemperature0p1C = s16Temperature0p1C;
            }
        }
        u8ValidTemperatureCount++;
    }

    /* Data0主MOS温度，Data1预放MOS温度，均为S8整数℃。 */
    /* Data2~Data3写最高电芯温度，单位0.1℃。 */
    ducar_can_write_s16_le(au8CanData, 2U, s16MaxCellTemperature0p1C);
    /* Data4~Data5写最低电芯温度，单位0.1℃。 */
    ducar_can_write_s16_le(au8CanData, 4U, s16MinCellTemperature0p1C);
    /* Data6写当前有效串数。 */
    au8CanData[6] = tBmsSnapshot.cell_count;
    /* Data7发送软件配置的电芯温度探头数量；故障或无效状态不改变配置数量。 */
    au8CanData[7] = (uint8_t)APP_BMS_MAX_TEMP_NUM;

    /* 发送F216。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F216),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

static uint16_t ducar_can_capacity_001ah(int32_t s32CapacityMah)
{
    /* 容量为负或为0时，协议不允许发送负容量，统一返回0。 */
    if (s32CapacityMah <= 0)
    {
        return 0U;
    }

    /* mAh除以10转换为0.01Ah，再限制到协议U16范围。 */
    return ducar_can_limit_u32_to_u16((uint32_t)s32CapacityMah / 10U);
}

/* F217：发送循环次数、FCC、剩余容量和半充次数。 */
static void ducar_can_send_f217(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取循环次数和容量相关快照。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /*
     * F217: 循环次数、FCC和剩余容量。SOC模块容量单位为mAh，协议为0.01Ah，
     * 因此除10。半充次数当前无业务计数，A0.1默认值定义为0。
     */
    /* Data0~Data1为循环次数。 */
    ducar_can_write_u16_le(au8CanData, 0U,
                            ducar_can_limit_u32_to_u16(tBmsSnapshot.soc.u32cycle_index));
    /* Data2~Data3为满充容量，内部mAh转换成0.01Ah。 */
    ducar_can_write_u16_le(au8CanData, 2U,
                            ducar_can_capacity_001ah(tBmsSnapshot.soc.s32battery_full_cap));
    /* Data4~Data5为剩余容量，内部mAh转换成0.01Ah。 */
    ducar_can_write_u16_le(au8CanData, 4U,
                            ducar_can_capacity_001ah(tBmsSnapshot.soc.s32remain_cap));
    /* Data6~Data7为当前没有业务源的半充次数，按协议默认0。 */
    ducar_can_write_u16_le(au8CanData, 6U, 0U);

    /* 发送F217。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F217),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F218：发送数字输入、MOS状态及按键状态位图。 */
static void ducar_can_send_f218(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取所有输入信号、MOS状态和按键在位标志。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* F218 byte0: 外部检测输入、三路MOS反馈和按键在位状态。 */
    /* bit0：DETD在位。 */
    if (tBmsSnapshot.signal.input.mcu_detd != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 0U);
    }
    /* bit1：DETC在位。 */
    if (tBmsSnapshot.signal.input.mcu_detc != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 1U);
    }
    /* A0.3 bit2=BMR、bit3=ID；本工程尚无已确认接入源，均报告未接入0。
     * DET_LOAD和MCU_2nd_DET分别是负载/二级熔断检测，不能冒充BMR/ID。 */
    /* bit4：放电MOS当前打开。 */
    if (tBmsSnapshot.mos.dsg_mos_state != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 4U);
    }
    /* bit5：充电MOS当前打开。 */
    if (tBmsSnapshot.mos.chg_mos_state != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 5U);
    }
    /* bit6：预放MOS当前打开。 */
    if (tBmsSnapshot.mos.pre_mos_state != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 6U);
    }
    /* bit7：KEY在位；由ECM_Status6 0x105最近3秒内的接收状态判断。 */
    if (tBmsSnapshot.signal.input.mcu_key_present != 0U)
    {
        au8CanData[0] |= (uint8_t)(1U << 7U);
    }

    /* 当前产品没有并联BMS：三路并联MOS状态均按协议“NA=3”编码。 */
    /* Data1/Data2当前没有并联BMS状态源，6个两位字段都编码为NA=3。 */
    au8CanData[1] = 0x3FU;
    au8CanData[2] = 0x3FU;
    /* Data3/Data4是并联充放电告警等级；当前无并联控制模块，按默认0发送。
     * 单机告警/保护分别在F21A~F21C、F21D~F21F报告。 */
    au8CanData[3] = 0U;
    au8CanData[4] = 0U;
    /* Data5：A0.7 BMS工作状态，直接复用统一状态机输出（0静置、1放电、2充电、3回馈）。 */
    au8CanData[5] = tBmsSnapshot.bms_work_status;
    /* Data6~Data7为保留字段，按0发送。 */
    au8CanData[6] = 0U;
    au8CanData[7] = 0U;

    /* 发送F218。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F218),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

static uint16_t ducar_can_current_01a_from_sop(int16_t s16Current01A)
{
    /* SOP负电流不作为可用充放电能力发送，统一下限为0。 */
    if (s16Current01A <= 0)
    {
        return 0U;
    }

    /* 源SOP电流单位0.1A，乘10转换为CAN的0.01A，并限制为U16。 */
    return ducar_can_limit_u32_to_u16((uint32_t)s16Current01A * 10U);
}

/* F219：充电需求和允许电流。 */
static void ducar_can_send_f219(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    /* 读取SOP允许值。 */
    app_bms_snapshot_read(&tBmsSnapshot);

    /* F219: 充电电压/充放电电流均使用SOP镜像，协议量纲为0.01V、0.01A。 */
    /* Data0~Data1为最大允许充电电压，mV转换为0.01V。 */
    ducar_can_write_u16_le(au8CanData, 0U,
                            (uint16_t)(tBmsSnapshot.sop.u32MaxChargeVoltageMv / 10U));
    /* Data2~Data3为最大充电电流。 */
    ducar_can_write_u16_le(au8CanData, 2U,
                            ducar_can_current_01a_from_sop(
                                tBmsSnapshot.sop.s16MaxChargeCurrent10mA));
    /* Data4~Data5为最大回充电流。 */
    ducar_can_write_u16_le(au8CanData, 4U,
                            ducar_can_limit_u32_to_u16(
                                (uint32_t)tBmsSnapshot.sop.u16MaxRechargeCurrent10mA * 10U));
    /* Data6~Data7为最大放电电流。 */
    ducar_can_write_u16_le(au8CanData, 6U,
                            ducar_can_current_01a_from_sop(
                                tBmsSnapshot.sop.s16MaxDischargeCurrent10mA));

    /* 发送F219。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F219),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F21A：电压、电芯和放电过流告警；保护由F21D单独发送。 */
static void ducar_can_send_f21a(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    uint16_t u16ChargeFaultA;
    uint16_t u16ChargeFaultB;
    uint16_t u16DischargeFaultA;
    uint16_t u16DischargeFaultB;
    uint16_t u16GeneralFaultA;

    /* 读取已经由故障模块汇总好的活动故障位图。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* 缓存充电方向A组故障位。 */
    u16ChargeFaultA = tBmsSnapshot.fault.active.detail.ucharge_fault_A.u16fault_A;
    /* 缓存充电方向B组故障位。 */
    u16ChargeFaultB = tBmsSnapshot.fault.active.detail.ucharge_fault_B.u16fault_B;
    /* 缓存放电方向A组故障位。 */
    u16DischargeFaultA = tBmsSnapshot.fault.active.detail.udischarge_fault_A.u16fault_A;
    /* 缓存放电方向B组告警位。 */
    u16DischargeFaultB = tBmsSnapshot.fault.active.detail.udischarge_fault_B.u16fault_B;
    /* 缓存公共故障A组位图。 */
    u16GeneralFaultA = tBmsSnapshot.fault.active.detail.ugeneral_fault_A.u16fault_A;

    /* F21A：只报告告警状态，保护位由F21D承担。 */
    /* Data0：单体过压告警。 */
    au8CanData[0] = ((u16ChargeFaultA & DUCAR_CAN_CHG_FAULT_A_COV_WARN) != 0U) ? 1U : 0U;
    /* Data1：严重过压告警，当前没有独立告警源。 */
    au8CanData[1] = 0U; /* 没有独立严重过压告警源。 */
    /* Data2：单体欠压告警。 */
    au8CanData[2] = ((u16DischargeFaultA & DUCAR_CAN_DSG_FAULT_A_CUV_WARN) != 0U) ? 1U : 0U;
    /* Data3：坏电芯告警，不能复用压差。 */
    au8CanData[3] = 0U; /* 电芯损坏无独立诊断源，不能拿压差冒充。 */
    /* Data4：电池包过压告警。 */
    au8CanData[4] = ((u16ChargeFaultB & DUCAR_CAN_CHG_FAULT_B_PACK_OV_WARN) != 0U) ? 1U : 0U;
    /* Data5：电池包欠压告警。 */
    au8CanData[5] = ((u16DischargeFaultB & DUCAR_CAN_DSG_FAULT_B_PACK_UV_WARN) != 0U) ? 1U : 0U;
    /* Data6：单体不平衡告警，使用压差告警位。 */
    au8CanData[6] = ((u16GeneralFaultA & DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_WARN) != 0U) ? 1U : 0U;
    /* Data7：放电过流告警。 */
    au8CanData[7] = ((u16DischargeFaultA & DUCAR_CAN_DSG_FAULT_A_OC_WARN) != 0U) ? 1U : 0U;

    /* 发送F21A告警帧。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21A),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F21B：电流、温度告警；保护和AFE故障由F21E单独发送。 */
static void ducar_can_send_f21b(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    uint16_t u16ChargeFaultA;
    uint16_t u16DischargeFaultA;
    uint16_t u16GeneralFaultA;

    /* 读取温度、电流和公共硬件故障位图。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* 保存充电方向故障位，供Data0/2/4使用。 */
    u16ChargeFaultA = tBmsSnapshot.fault.active.detail.ucharge_fault_A.u16fault_A;
    /* 保存放电方向故障位，供Data1/3使用。 */
    u16DischargeFaultA = tBmsSnapshot.fault.active.detail.udischarge_fault_A.u16fault_A;
    /* 保存公共故障位，供Data5和后续硬件故障位使用。 */
    u16GeneralFaultA = tBmsSnapshot.fault.active.detail.ugeneral_fault_A.u16fault_A;

    /* F21B：充放电温度/电流告警，不含AFE保护。 */
    /* Data0：充电过流告警。 */
    au8CanData[0] = ((u16ChargeFaultA & DUCAR_CAN_CHG_FAULT_A_OC_WARN) != 0U) ? 1U : 0U;
    /* Data1：放电过温告警和保护。 */
    au8CanData[1] = ((u16DischargeFaultA & DUCAR_CAN_DSG_FAULT_A_OT_WARN) != 0U) ? 1U : 0U;
    /* Data2：充电过温告警和保护。 */
    au8CanData[2] = ((u16ChargeFaultA & DUCAR_CAN_CHG_FAULT_A_OT_WARN) != 0U) ? 1U : 0U;
    /* Data3：放电欠温告警和保护。 */
    au8CanData[3] = ((u16DischargeFaultA & DUCAR_CAN_DSG_FAULT_A_UT_WARN) != 0U) ? 1U : 0U;
    /* Data4：充电欠温告警和保护。 */
    au8CanData[4] = ((u16ChargeFaultA & DUCAR_CAN_CHG_FAULT_A_UT_WARN) != 0U) ? 1U : 0U;
    /* Data5：MOS过温告警和保护。 */
    au8CanData[5] = ((u16GeneralFaultA & DUCAR_CAN_GENERAL_FAULT_MOS_OT_WARN) != 0U) ? 1U : 0U;
    /* 当前项目没有独立温差告警源，byte6保持无告警。 */
    /* Data6：当前没有独立温差故障源，按正常值0发送。 */
    au8CanData[6] = 0U;

    /* A0.6 Data7：回馈电流告警。 */
    au8CanData[7] = ((u16ChargeFaultA & DUCAR_CAN_CHG_FAULT_A_RECHARGE_WARN) != 0U) ? 1U : 0U;
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21B),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F21C：环境与低SOC告警。 */
static void ducar_can_send_f21c(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    uint16_t u16DischargeFaultA;

    /* 读取放电故障和公共故障汇总。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* 保存放电方向故障位。 */
    u16DischargeFaultA = tBmsSnapshot.fault.active.detail.udischarge_fault_A.u16fault_A;

    /* F21C：当前没有环境过温独立源，未定义字段保持0。 */
    /* Data0：当前无环境温度故障源。 */
    au8CanData[0] = 0U;
    /* Data1：低SOC故障标志。 */
    au8CanData[1] = ((u16DischargeFaultA & DUCAR_CAN_DSG_FAULT_A_LOW_SOC) != 0U) ? 1U : 0U;
    /* Data2：当前无充电器通信故障源。 */
    au8CanData[2] = 0U;
    /* Data3的MOS失效属于保护，F21C告警位保持0。 */
    /* Data4~Data7为当前没有独立来源的保留故障位，全部保持0。 */
    au8CanData[4] = 0U;
    au8CanData[5] = 0U;
    au8CanData[6] = 0U;
    au8CanData[7] = 0U;

    /* 发送F21C告警帧。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21C),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F21D：电压、电芯和放电过流保护；每项只读取保护位。 */
static void ducar_can_send_f21d(void)
{
    app_bms_snapshot_t s;
    uint8_t d[8] = {0};
    app_bms_snapshot_read(&s);
    d[0] = (s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_COV_PRO) ? 1U : 0U;
    /* 严重过压为独立布尔字段：有保护=1，不使用等级值2。 */
    d[1] = (s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_COV_PRO2) ? 1U : 0U;
    /* 单体欠压：一级=1、二级=2、深度欠压=3，同时出现时报告最高等级。 */
    d[2] = (s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_CUV_DEEP) ? 3U :
           ((s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_CUV_PRO2) ? 2U :
           ((s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_CUV_PRO1) ? 1U : 0U));
    /* Data3电芯损坏无独立保护源。 */
    d[4] = (s.fault.active.detail.ucharge_fault_B.u16fault_B & DUCAR_CAN_CHG_FAULT_B_PACK_OV_PRO) ? 1U : 0U;
    d[5] = (s.fault.active.detail.udischarge_fault_B.u16fault_B & DUCAR_CAN_DSG_FAULT_B_PACK_UV_PRO) ? 1U : 0U;
    d[6] = (s.fault.active.detail.ugeneral_fault_A.u16fault_A &
            (DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_PRO | DUCAR_CAN_GENERAL_FAULT_CELL_DIFF_PRO2)) ? 1U : 0U;
    /* A0.6：各级放电过流报告实际最高等级；锁定状态由F21F单独报告。 */
    d[7] = (s.fault.active.detail.udischarge_fault_B.u16fault_B & DUCAR_CAN_DSG_FAULT_B_OC_PRO4) ? 4U :
           ((s.fault.active.detail.udischarge_fault_B.u16fault_B & DUCAR_CAN_DSG_FAULT_B_OC_PRO3) ? 3U :
           ((s.fault.active.detail.udischarge_fault_B.u16fault_B & DUCAR_CAN_DSG_FAULT_B_OC_PRO2) ? 2U :
           ((s.fault.active.detail.udischarge_fault_B.u16fault_B & DUCAR_CAN_DSG_FAULT_B_OC_PRO1) ? 1U : 0U)));
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21D),
                     D_CAN_ID_EXTENDED, d, sizeof(d));
}

/* F21E：电流、温度保护及AFE故障1。 */
static void ducar_can_send_f21e(void)
{
    app_bms_snapshot_t s;
    uint8_t d[8] = {0};
    app_bms_snapshot_read(&s);
    /* 回馈过流映射为一级充电过流保护；多级普通过流同时置位时优先报告更高级别。 */
    d[0] = (s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_OC_PRO2) ? 2U :
           ((s.fault.active.detail.ucharge_fault_A.u16fault_A &
             (DUCAR_CAN_CHG_FAULT_A_OC_PRO1 | DUCAR_CAN_CHG_FAULT_A_RECHARGE_OCC)) ? 1U : 0U);
    d[1] = (s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_OT_PRO) ? 1U : 0U;
    d[2] = (s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_OT_PRO) ? 1U : 0U;
    d[3] = (s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_UT_PRO) ? 1U : 0U;
    d[4] = (s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_UT_PRO) ? 1U : 0U;
    d[5] = (s.fault.active.detail.ugeneral_fault_A.u16fault_A & DUCAR_CAN_GENERAL_FAULT_MOS_OT_PRO) ? 1U : 0U;
    /* Data6温差保护无独立源。 */
    d[7] = ((s.afe_fault.cell_ov != 0U) ? 1U : 0U) |
           ((s.afe_fault.cell_uv != 0U) ? 4U : 0U) |
           (((s.afe_fault.dsg_oc1 != 0U) || (s.afe_fault.dsg_oc2 != 0U)) ? 8U : 0U) |
           (((s.afe_fault.chg_oc1 != 0U) || (s.afe_fault.chg_oc2 != 0U)) ? 16U : 0U) |
           ((s.afe_fault.short_circuit != 0U) ? 32U : 0U);
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21E),
                     D_CAN_ID_EXTENDED, d, sizeof(d));
}

/* F21F：环境、低SOC、AFE故障2、MOS、回馈保护及其他保护位。 */
static void ducar_can_send_f21f(void)
{
    app_bms_snapshot_t s;
    uint8_t d[8] = {0};
    app_bms_snapshot_read(&s);
    /* Data0环境过温保护无独立源。 */
    d[1] = (s.fault.active.detail.udischarge_fault_A.u16fault_A & DUCAR_CAN_DSG_FAULT_A_LOW_SOC) ? 1U : 0U;
    /* Data2 AFE故障2在本硬件上没有已定义的独立映射。 */
    /* Data3 BIT0/1分别报告充电/放电MOS损坏保护，与宗申0x89共用最终故障标志。 */
    d[3] = ((s.fault.active.detail.ugeneral_fault_A.u16fault_A & DUCAR_CAN_GENERAL_FAULT_CMOS_FAIL) != 0U)
               ? DUCAR_CAN_F21F_CHG_MOS_FAIL_PROTECT : 0U;
    if ((s.fault.active.detail.ugeneral_fault_A.u16fault_A & DUCAR_CAN_GENERAL_FAULT_DMOS_FAIL) != 0U)
        d[3] |= DUCAR_CAN_F21F_DSG_MOS_FAIL_PROTECT;
    /* A0.6 Data4：回馈电流保护等级。 */
    d[4] = ((s.fault.active.detail.ucharge_fault_A.u16fault_A & DUCAR_CAN_CHG_FAULT_A_RECHARGE_OCC) != 0U) ? 1U : 0U;
    /* Data5 BIT2：充电过流锁；BIT3：放电过流锁。 */
    d[5] = ((s.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock != 0U) ? 0x04U : 0U) |
           ((s.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U) ? 0x08U : 0U);
    /* Data5 BIT0/1当前无通信总线故障/地址冲突源；Data6~7保留为0。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F21F),
                     D_CAN_ID_EXTENDED, d, sizeof(d));
}

/*
 * F220欠压点根据最低有效电芯温度作滞回选择：
 * 常温状态在最低温度<=-1.0℃时切入低温值；低温状态在最低温度>=+1.0℃时恢复常温值。
 * 没有有效温度时保留当前选择，避免采样失效导致阈值跳变。
 */
static void ducar_can_f220_update_low_temp_uv_state(const app_bms_snapshot_t *ptBmsSnapshot)
{
    uint8_t u8TemperatureIndex;
    uint8_t u8HasValidTemperature = 0U;
    int16_t s16MinimumTemperature0p1c = 0;

    if (ptBmsSnapshot == NULL)
    {
        return;
    }

    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < APP_BMS_MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        if ((ptBmsSnapshot->temp_valid_mask & (uint8_t)(1U << u8TemperatureIndex)) == 0U)
        {
            continue;
        }

        if ((u8HasValidTemperature == 0U) ||
            (ptBmsSnapshot->temp_0p1c[u8TemperatureIndex] < s16MinimumTemperature0p1c))
        {
            s16MinimumTemperature0p1c = ptBmsSnapshot->temp_0p1c[u8TemperatureIndex];
            u8HasValidTemperature = 1U;
        }
    }

    if (u8HasValidTemperature == 0U)
    {
        return;
    }

    if (u8CanF220LowTempUvActive == 0U)
    {
        if (s16MinimumTemperature0p1c <= DUCAR_CAN_F220_LOW_TEMP_ENTER_0P1C)
        {
            u8CanF220LowTempUvActive = 1U;
        }
    }
    else if (s16MinimumTemperature0p1c >= DUCAR_CAN_F220_LOW_TEMP_EXIT_0P1C)
    {
        u8CanF220LowTempUvActive = 0U;
    }
}

/* F220 Data0~1：配置值单位mV，协议原始值单位0.1V，0xFFFF保留为无效值。 */
static uint16_t ducar_can_f220_controller_uv_0p1v(const app_bms_snapshot_t *ptBmsSnapshot)
{
    uint32_t u32VoltageMv;
    uint32_t u32ProtocolValue;

    ducar_can_f220_update_low_temp_uv_state(ptBmsSnapshot);
    u32VoltageMv = (u8CanF220LowTempUvActive != 0U) ?
                    (uint32_t)CONTROLLER_UNDER_VOLTAGE_PROTECT_MINUS_0_TMEP :
                    (uint32_t)CONTROLLER_UNDER_VOLTAGE_PROTECT;
    u32ProtocolValue = u32VoltageMv / 100UL;

    return (u32ProtocolValue >= (uint32_t)DUCAR_CAN_INVALID_U16) ?
           (uint16_t)(DUCAR_CAN_INVALID_U16 - 1U) : (uint16_t)u32ProtocolValue;
}

/* F220的事件次数使用U16；0xFFFF是协议无效值，实际计数在此处饱和到0xFFFE。 */
static uint16_t ducar_can_f220_event_count_u16(uint32_t u32EventCount)
{
    return (u32EventCount >= (uint32_t)DUCAR_CAN_INVALID_U16) ?
           (uint16_t)(DUCAR_CAN_INVALID_U16 - 1U) : (uint16_t)u32EventCount;
}

/* F220：发送控制器欠压保护点、过充次数、过放次数和充电剩余时间。 */
static void ducar_can_send_f220(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    struct _app_bms_fault_t tBmsFaultSnapshot;
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};

    app_bms_snapshot_read(&tBmsSnapshot);
    app_bms_fault_snapshot_read(&tBmsFaultSnapshot);

    /* Data0~Data1：控制器欠压保护点，0.1V/bit。 */
    ducar_can_write_u16_le(au8CanData, 0U,
                           ducar_can_f220_controller_uv_0p1v(&tBmsSnapshot));
    /* Data2~Data3：过充保护事件次数。 */
    ducar_can_write_u16_le(au8CanData, 2U,
                           ducar_can_f220_event_count_u16(tBmsFaultSnapshot.u32overcharge_count));
    /* Data4~Data5：过放保护事件次数。 */
    ducar_can_write_u16_le(au8CanData, 4U,
                           ducar_can_f220_event_count_u16(tBmsFaultSnapshot.u32overdischarge_count));
    /* Data6~Data7：充电剩余时间，单位分钟；尚无估算值时为0xFFFF。 */
    ducar_can_write_u16_le(au8CanData, 6U, app_charge_time_get_minutes());

    /* 发送F220。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F220),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

/* F221：发送当前已成功应用到AFE的逻辑电芯均衡位图。 */
static void ducar_can_send_f221(void)
{
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC] = {0};
    uint32_t u32BalanceMask = app_balance_get_mask();

    /* Data0~Data3为U32小端，bit0对应第1节；上层掩码已按AFE接线映射回逻辑串号。 */
    au8CanData[0] = (uint8_t)u32BalanceMask;
    au8CanData[1] = (uint8_t)(u32BalanceMask >> 8U);
    au8CanData[2] = (uint8_t)(u32BalanceMask >> 16U);
    au8CanData[3] = (uint8_t)(u32BalanceMask >> 24U);
    /* Data4~Data7为保留区，沿用协议无效填充值0xFF。 */
    ducar_can_write_u16_le(au8CanData, 4U, DUCAR_CAN_INVALID_U16);
    ducar_can_write_u16_le(au8CanData, 6U, DUCAR_CAN_INVALID_U16);

    /* 发送F221。 */
    (void)d_can_send(&can0, ducar_can_build_bms_data_id(DUCAR_CAN_PGN_F221),
                     D_CAN_ID_EXTENDED, au8CanData, DUCAR_CAN_DATA_DLC);
}

void ducar_can_protocol_init(void)
{
    s_cal2_reply_head = 0U;
    s_cal2_reply_count = 0U;
    s_cal2_can_ms = 0U;
    /* 上电或CAN重新初始化时，清空1ms基准计数。 */
    u16Can1msCount = 0U;
    /* 清空DETD触发的主动发送剩余时间。 */
    u16DetdProactiveTxTicks = 0U;
    /* 清空LifeValue计时。 */
    u16CanLifeValueTicks = 0U;
    /* LifeValue从0开始递增。 */
    u8CanLifeValue = 0U;
    /* 默认认为DETD未在位，等待下一次有效上升沿。 */
    u8DetdConnectedLast = 0U;
    /* F220上电默认使用常温欠压点，随后由有效电芯温度按滞回切换。 */
    u8CanF220LowTempUvActive = 0U;
    /* 默认允许总线通信。 */
    u8CanBusSilent = 0U;
    /* CAN IAP静默状态在协议初始化后关闭。 */
    u8CanIapSilenceActive = 0U;
    /* 清除待发送ACK标志。 */
    u8CanAckPending = 0U;
    /* 没有ACK时默认状态为NAK，防止未初始化值被发送。 */
    u8CanAckStatus = DUCAR_CAN_NAK;
    /* 默认ACK目标为PC地址。 */
    u8CanAckDestination = DUCAR_CAN_PC_ADDRESS;
    /* 清除待确认的功能码。 */
    u16CanAckFunctionCode = 0U;
    /* 清除ACK发送后动作。 */
    eCanActionAfterAck = DUCAR_CAN_ACTION_NONE;
    /* 清除已经发出ACK、等待邮箱完成的动作。 */
    eCanActionWaitingTx = DUCAR_CAN_ACTION_NONE;
    /* 清除动作等待超时计数。 */
    u8CanActionWaitTicks = 0U;
    /* TP状态回到空闲，不保留上一次长报文会话。 */
    eCanTpState = DUCAR_CAN_TP_IDLE;
    /* 清除TP有效数据长度。 */
    u8CanTpDataLength = 0U;
    /* 清除TP总包数。 */
    u8CanTpPacketCount = 0U;
    /* 清除TP下一个待发送序号。 */
    u8CanTpNextPacket = 0U;
    /* 清除本次CTS允许发送的窗口大小。 */
    u8CanTpBurstPackets = 0U;
    /* 默认TP目标为PC。 */
    u8CanTpDestination = DUCAR_CAN_PC_ADDRESS;
    /* 清除TP等待CTS/EOM计时。 */
    u16CanTpWaitTicks = 0U;
    /* 清除当前TP会话PGN。 */
    u16CanTpPgn = 0U;
    u16CanLogRequested = 0U;
    u16CanLogNext = 0U;
    u8CanSocOcvAckPending = 0U;
    u8CanSocOcvAckDestination = DUCAR_CAN_PC_ADDRESS;
    u16CanSocOcvWaitTicks = 0U;
    u8CanSocWriteAckPending = 0U;
    u8CanSocWriteAckDestination = DUCAR_CAN_PC_ADDRESS;
    u8CanTpRxLength = 0U;
    u8CanTpRxPackets = 0U;
    u8CanTpRxNext = 0U;
    u16CanTpRxWaitTicks = 0U;
}

void ducar_can_protocol_set_iap_silence(bool enabled)
{
    /* 同步封锁底层所有普通CAN发送，并取消升级开始前已提交的普通邮箱。 */
    d_can_set_iap_silence(enabled);

    if (enabled)
    {
        if (u8CanIapSilenceActive != 0U)
        {
            return;
        }

        /* 仅在进入升级静默时清理普通协议的待发回复，避免升级结束后补发旧数据。 */
        u8CanIapSilenceActive = 1U;
        s_cal2_reply_head = 0U;
        s_cal2_reply_count = 0U;
        u8CanAckPending = 0U;
        eCanActionAfterAck = DUCAR_CAN_ACTION_NONE;
        eCanActionWaitingTx = DUCAR_CAN_ACTION_NONE;
        u8CanActionWaitTicks = 0U;
        eCanTpState = DUCAR_CAN_TP_IDLE;
        u8CanTpDataLength = 0U;
        u8CanTpPacketCount = 0U;
        u8CanTpNextPacket = 0U;
        u8CanTpBurstPackets = 0U;
        u16CanTpWaitTicks = 0U;
        u16CanTpPgn = 0U;
        u16CanLogRequested = 0U;
        u16CanLogNext = 0U;
        u8CanSocOcvAckPending = 0U;
        u16CanSocOcvWaitTicks = 0U;
        u8CanSocWriteAckPending = 0U;
    }
    else
    {
        u8CanIapSilenceActive = 0U;
    }
}

void ducar_can_data_renew(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t u8DetdConnected;

    /* 从总快照读取已经消抖后的DETD状态。 */
    app_bms_snapshot_read(&tBmsSnapshot);
    /* 将结构体中的布尔量规范化成0或1，便于边沿比较。 */
    u8DetdConnected = (tBmsSnapshot.signal.input.mcu_detd != 0U) ? 1U : 0U;

    /* 只有DETD经500ms去抖后的未接入→接入沿，能装载一次完整5秒主动发送窗口。 */
    /* 新状态为在位且旧状态为不在位时，认定发生一次DETD接入事件。 */
    if ((u8DetdConnected != 0U) && (u8DetdConnectedLast == 0U))
    {
        /* 5秒窗口按1ms任务节拍装载5000个计数。 */
        u16DetdProactiveTxTicks = DUCAR_CAN_DETD_TX_WINDOW_TICKS;
    }

    /* 保存本次状态，供下一次调用进行边沿比较。 */
    u8DetdConnectedLast = u8DetdConnected;
}

void ducar_can_bus_timer_handle(void)
{
    ++s_cal2_can_ms;
    app_current_calib_cal2_advance_ms(1U);
    /* Comm_Task每1ms调用一次；基准计数始终推进，静默/窗口外也不暂停。 */
    if (++u16Can1msCount >= (DUCAR_CAN_CYCLE_1000MS * 2U))
    {
        u16Can1msCount = 0U;
    }
    if ((u8CanTpRxNext != 0U) &&
        (++u16CanTpRxWaitTicks >= DUCAR_CAN_CYCLE_500MS * 10U))
    {
        u8CanTpRxNext = 0U;
        u16CanTpRxWaitTicks = 0U;
    }
    /* PF=E0控制的静默状态没有隐式超时；接收函数是唯一的修改点。 */
    /* 每经过一次1ms任务，LifeValue计时加一。 */
    if (++u16CanLifeValueTicks >= DUCAR_CAN_CYCLE_1000MS)
    {
        /* 达到1秒后重新计时。 */
        u16CanLifeValueTicks = 0U;
        /* LifeValue按字节自然溢出循环递增。 */
        u8CanLifeValue++;
    }

    /* 所有活跃TP阶段均有5秒无进展超时；发送邮箱堵塞也不能永久占用会话。 */
    if (eCanTpState != DUCAR_CAN_TP_IDLE)
    {
        /* 累加当前TP等待时长。 */
        if (++u16CanTpWaitTicks >= DUCAR_CAN_CYCLE_500MS * 10U)
        {
            /* 5秒未收到有效控制帧或未成功发送数据时释放会话，允许后续查询重试。 */
            eCanTpState = DUCAR_CAN_TP_IDLE;
            u16CanTpWaitTicks = 0U;
            u16CanLogNext = 0U;
        }
    }
}

bool ducar_can_detd_tx_window_is_active(void)
{
    /* 每调用一次消耗一个1ms窗口计数；静默仅抑制发送，不改变窗口寿命。 */
    /* 计数为0表示DETD触发窗口已经结束。 */
    if (u16DetdProactiveTxTicks == 0U)
    {
        return false;
    }

    /* 先消耗一个本次发送周期的窗口配额。 */
    u16DetdProactiveTxTicks--;

    /* 静默只抑制本轮发送，不会清掉或重装DETD的5秒计数。 */
    return (u8CanBusSilent == 0U);
}

void ducar_can_50ms_task(void)
{
    /* 50ms槽只发送F218，承载输入信号和MOS状态。 */

    /* 当前槽命中50ms节拍时发送F218。 */
    if ((u16Can1msCount % DUCAR_CAN_CYCLE_50MS) == 0U)
    {
        ducar_can_send_f218();
    }
}

void ducar_can_100ms_task(void)
{
    /* 100ms槽发送F20D/F20E、F219及A0.2的告警和保护帧。 */
    /* 每个分支使用不同偏移，把多个同周期报文分散到不同1ms槽。 */
    if (((u16Can1msCount + 1U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        /* 第一个100ms槽发送F20D。 */
        ducar_can_send_f20d();
    }
    else if (((u16Can1msCount + 2U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        /* 第二个100ms槽发送F20E。 */
        ducar_can_send_f20e();
    }
    else if (((u16Can1msCount + 3U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        /* 第三个100ms槽发送F21A。 */
        ducar_can_send_f21a();
    }
    else if (((u16Can1msCount + 4U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        /* 第四个100ms槽发送F21B。 */
        ducar_can_send_f21b();
    }
    else if (((u16Can1msCount + 5U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        /* 第五个100ms槽发送F21C。 */
        ducar_can_send_f21c();
    }
    else if (((u16Can1msCount + 6U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        ducar_can_send_f21d();
    }
    else if (((u16Can1msCount + 7U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        ducar_can_send_f21e();
    }
    else if (((u16Can1msCount + 8U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        ducar_can_send_f21f();
    }
    else if (((u16Can1msCount + 9U) % DUCAR_CAN_CYCLE_100MS) == 0U)
    {
        ducar_can_send_f219();
    }
}

void ducar_can_200ms_task(void)
{
    /* 200ms槽依次错开发送F20F~F214单体电压报文。 */
    /* 200ms周期内按偏移依次发送6组单体电压。 */
    if (((u16Can1msCount + 9U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F20F。 */
        ducar_can_send_f20f();
    }
    else if (((u16Can1msCount + 10U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F210。 */
        ducar_can_send_f210();
    }
    else if (((u16Can1msCount + 11U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F211。 */
        ducar_can_send_f211();
    }
    else if (((u16Can1msCount + 12U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F212。 */
        ducar_can_send_f212();
    }
    else if (((u16Can1msCount + 13U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F213。 */
        ducar_can_send_f213();
    }
    else if (((u16Can1msCount + 14U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* 发送F214。 */
        ducar_can_send_f214();
    }
    else if (((u16Can1msCount + 15U) % DUCAR_CAN_CYCLE_200MS) == 0U)
    {
        /* A0.5 F221均衡状态每200ms发送一次。 */
        ducar_can_send_f221();
    }
}

void ducar_can_500ms_task(void)
{
    /* 500ms槽只发送F215~F217温度和容量报文。 */
    if (((u16Can1msCount + 15U) % DUCAR_CAN_CYCLE_500MS) == 0U)
    {
        /* 发送F215。 */
        ducar_can_send_f215();
    }
    else if (((u16Can1msCount + 16U) % DUCAR_CAN_CYCLE_500MS) == 0U)
    {
        /* 发送F216。 */
        ducar_can_send_f216();
    }
    else if (((u16Can1msCount + 17U) % DUCAR_CAN_CYCLE_500MS) == 0U)
    {
        /* 发送F217。 */
        ducar_can_send_f217();
    }
}

void ducar_can_1000ms_task(void)
{
    /* A0.3 F220控制器欠压保护点/统计报文按1000ms发送。 */
    if (((u16Can1msCount + 26U) % DUCAR_CAN_CYCLE_1000MS) == 0U)
    {
        ducar_can_send_f220();
    }
}

static bool ducar_can_send_ack(uint8_t u8Status,
                               uint8_t u8Destination,
                               uint16_t u16FunctionCode)
{
    /* 构造E8确认帧：data0为ACK/NAK，data6/7为被确认的PF/PGN。 */
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC];

    /* 未使用字节按协议填0xFF，避免发送未初始化内容。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0表示ACK或NAK结果。 */
    au8CanData[0] = u8Status;
    /* Data1~Data5未使用，保留0xFF填充；Data6/7写被确认PGN。 */
    /* Data6携带被确认PF/PGN的低字节。 */
    au8CanData[6] = (uint8_t)(u16FunctionCode & 0xFFU);
    /* Data7携带被确认PF/PGN的高字节。 */
    au8CanData[7] = (uint8_t)(u16FunctionCode >> 8U);

    /* ACK帧目标为触发该命令的源地址，源地址固定为BMS_A。 */
    return d_can_send(&can0,
                      ducar_can_id_encode(DUCAR_CAN_PF_ACK,
                                          u8Destination,
                                          DUCAR_CAN_BMS_A_ADDRESS),
                      D_CAN_ID_EXTENDED,
                      au8CanData,
                      DUCAR_CAN_DATA_DLC);
}

static bool ducar_can_queue_ack(uint8_t u8Status,
                               uint8_t u8Destination,
                               uint16_t u16FunctionCode,
                               ducar_can_action_t eActionAfterAck)
{
    /* 只保留一个待发送ACK，避免多个控制动作同时排队导致顺序不确定。 */
    /* 已有ACK或上一个动作尚未完成时，拒绝新的控制应答。 */
    if ((u8CanAckPending != 0U) ||
        (eCanActionWaitingTx != DUCAR_CAN_ACTION_NONE))
    {
        return false;
    }

    /* 保存应答结果，稍后由控制回复任务实际发送。 */
    u8CanAckStatus = u8Status;
    /* 保存ACK目标地址。 */
    u8CanAckDestination = u8Destination;
    /* 保存被确认的PF/PGN。 */
    u16CanAckFunctionCode = u16FunctionCode;
    /* 保存ACK发送完成后需要执行的动作。 */
    eCanActionAfterAck = eActionAfterAck;
    /* 标记有待发送ACK。 */
    u8CanAckPending = 1U;
    /* 从0开始统计ACK发送后等待时间。 */
    u8CanActionWaitTicks = 0U;
    return true;
}

/*
 * 将配置字符串中的数字部分转换为协议版本数值；各字段传入自己的上限。
 * 例如“V001”转换为1，“T9”转换为9；没有数字或空指针时返回0。
 */
static uint16_t ducar_can_version_to_number(const char *pszVersion, uint16_t u16Maximum)
{
    /* 先用32位计算再与字段上限比较，避免U16累加提前回绕。 */
    uint32_t u32VersionNumber = 0U;

    /* 没有版本字符串时返回协议默认值0。 */
    if (pszVersion == 0)
    {
        return 0U;
    }

    /* 遍历字符串，允许输入带V/T前缀，只提取其中的数字。 */
    while (*pszVersion != '\0')
    {
        /* 只有ASCII数字参与版本号计算。 */
        if ((*pszVersion >= '0') && (*pszVersion <= '9'))
        {
            /* 十进制累加当前数字。 */
            u32VersionNumber = u32VersionNumber * 10U + (uint32_t)(*pszVersion - '0');
            /* V/HW字段上限255，T字段上限65535；越界饱和而不截断。 */
            if (u32VersionNumber > u16Maximum)
            {
                return u16Maximum;
            }
        }
        /* 指向下一个字符串字符。 */
        pszVersion++;
    }

    return (uint16_t)u32VersionNumber;
}

/*
 * 启动普通协议的TP发送。
 * 这里只缓存待发送数据并进入SEND_RTS状态，真正的RTS/DT发送由周期任务完成，
 * 这样可以把请求处理和CAN邮箱发送解耦，并等待对端CTS控制发送窗口。
 */
static bool ducar_can_tp_start(uint8_t u8Destination,
                               uint16_t u16Pgn,
                               const uint8_t *pau8Data,
                               uint8_t u8DataLength)
{
    /* 同一时刻只允许一个普通协议TP会话。 */
    if ((eCanTpState != DUCAR_CAN_TP_IDLE) ||
        /* 调用方必须提供待发送数据。 */
        (pau8Data == 0) ||
        /* 8字节以内不需要TP，应该走单帧发送。 */
        (u8DataLength <= DUCAR_CAN_MAX_DATA_LEN) ||
        /* 超过内部缓存上限的数据不能建立会话。 */
        (u8DataLength > DUCAR_CAN_TP_MAX_DATA_LEN))
    {
        return false;
    }

    /* 把待发送数据复制到协议层缓存，避免调用方局部变量失效。 */
    memcpy(au8CanTpData, pau8Data, u8DataLength);
    /* 保存总长度。 */
    u8CanTpDataLength = u8DataLength;
    /* 每个DT携带7字节有效数据，向上取整得到DT总包数。 */
    u8CanTpPacketCount = (uint8_t)((u8DataLength + 6U) / 7U);
    /* TP序号从1开始。 */
    u8CanTpNextPacket = 1U;
    /* 等待CTS期间还没有可发送窗口。 */
    u8CanTpBurstPackets = 0U;
    /* 保存本次TP目标地址。 */
    u8CanTpDestination = u8Destination;
    /* 清除本次TP等待计时。 */
    u16CanTpWaitTicks = 0U;
    /* 保存本次TP对应的PGN，后续校验CTS/EOM ACK必须匹配。 */
    u16CanTpPgn = u16Pgn;
    /* 下一次周期任务先发送RTS。 */
    eCanTpState = DUCAR_CAN_TP_SEND_RTS;
    return true;
}

/*
 * 发送查询响应：不超过8字节时直接发单帧，超过8字节时转入TP发送状态机。
 */
static bool ducar_can_send_requested_data(uint8_t u8Destination,
                                          uint16_t u16Pgn,
                                          const uint8_t *pau8Data,
                                          uint8_t u8DataLength)
{
    /* 数据指针和长度必须有效。 */
    if ((pau8Data == 0) ||
        (u8DataLength == 0U) ||
        (u8DataLength > DUCAR_CAN_TP_MAX_DATA_LEN))
    {
        return false;
    }

    /* 大于单帧容量时，进入TP状态机等待RTS发送。 */
    if (u8DataLength > DUCAR_CAN_MAX_DATA_LEN)
    {
        return ducar_can_tp_start(u8Destination,
                                  u16Pgn,
                                  pau8Data,
                                  u8DataLength);
    }

    /* 单帧响应直接组装BMS源地址ID并提交到底层发送队列。 */
    return d_can_send(&can0,
                      ducar_can_id_encode((uint8_t)(u16Pgn >> 8U),
                                          (uint8_t)u16Pgn,
                                          DUCAR_CAN_BMS_A_ADDRESS),
                      D_CAN_ID_EXTENDED,
                      pau8Data,
                      u8DataLength);
}

/*
 * 处理PF=EF查询报文。
 * Data0/1为请求PGN，Data2为查询控制字节；响应内容按PGN选择，
 * 对不具备数据源或不支持的PGN统一排队NAK。
 */
static void ducar_can_receive_request(const t_can_packet *ptCanPacket,
                                      const ducar_can_id_fields_t *ptCanIdFields)
{
    uint16_t u16RequestedPgn;
    uint8_t au8ResponseData[DUCAR_CAN_TP_MAX_DATA_LEN];
    uint8_t u8ResponseLength;
    const char *pszParameter;
    size_t uParameterLength;
    d_rtc_time_t tRtcTime;

    /* 查询报文至少需要2字节PGN和1字节控制码。 */
    if ((ptCanPacket->u8len < 3U) ||
        /* 查询只能由PC发起。 */
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        /* 目标只能是本BMS或全局地址。 */
        ((ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS) &&
         (ptCanIdFields->u8Ps != DUCAR_CAN_GLOBAL_ADDRESS)))
    {
        return;
    }

    /* Data0是PGN低字节，Data1是PGN高字节，按小端组合。 */
    u16RequestedPgn = (uint16_t)ptCanPacket->au8data[0] |
                      (uint16_t)((uint16_t)ptCanPacket->au8data[1] << 8U);
    if (u16RequestedPgn == DUCAR_CAN_PGN_F233)
    {
        static const uint8_t capability[8] = {'C', 'A', 'L', '2', 1U, 1U, 0xFFU, 0xFFU};
        /* 能力查询只能定向到本PACK，不能用广播或附带额外字节。 */
        if ((ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS) &&
            (ptCanPacket->u8len == 3U) && (ptCanPacket->au8data[2] == 0U))
        {
            (void)ducar_can_send_requested_data(ptCanIdFields->u8Sa,
                                                DUCAR_CAN_PGN_F233,
                                                capability, 8U);
        }
        return;
    }
    /* F232的Data2是子命令；其他查询只接受Data2=0。 */
    if ((ptCanPacket->au8data[2] != 0U) &&
        (u16RequestedPgn != DUCAR_CAN_PGN_F232))
    {
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16RequestedPgn,
                                  DUCAR_CAN_ACTION_NONE);
        return;
    }

    /* 所有通过来源、目标和格式检查的PC查询都续期，不依赖查询的PGN是否受支持。 */
    u16DetdProactiveTxTicks = DUCAR_CAN_DETD_TX_WINDOW_TICKS;

    /* 根据请求PGN准备对应响应数据和长度。 */
    switch (u16RequestedPgn)
    {
        case DUCAR_CAN_PGN_F232:
            if (app_current_calib_cal2_is_active())
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK, ptCanIdFields->u8Sa,
                                          u16RequestedPgn, DUCAR_CAN_ACTION_NONE);
                return;
            }
            if ((ptCanPacket->au8data[2] == 0x20U) &&
                (ptCanPacket->u8len == 3U))
            {
                uint16_t count = app_log_rtd_num();
                au8ResponseData[0] = (count > 255U) ? 255U : (uint8_t)count;
                u8ResponseLength = 1U;
                break;
            }
            if ((ptCanPacket->au8data[2] == 0x01U) &&
                (ptCanPacket->u8len == 5U) &&
                (eCanTpState == DUCAR_CAN_TP_IDLE))
            {
                uint16_t count = (uint16_t)ptCanPacket->au8data[3] |
                                 ((uint16_t)ptCanPacket->au8data[4] << 8U);
                if ((count != 0U) && (count <= app_log_rtd_num()))
                {
                    u16CanLogRequested = count;
                    u16CanLogNext = 1U;
                    ducar_can_log_next();
                    return;
                }
            }
            (void)ducar_can_queue_ack(DUCAR_CAN_NAK, ptCanIdFields->u8Sa,
                                      u16RequestedPgn, DUCAR_CAN_ACTION_NONE);
            return;
        case DUCAR_CAN_PGN_F200:
            /* V/T/HW/协议版本依次占Data0~Data3，Data4~7保留FF。 */
            memset(au8ResponseData, 0xFF, DUCAR_CAN_DATA_DLC);
            au8ResponseData[0] = (uint8_t)ducar_can_version_to_number(
                app_param_get_package_v_version(), UINT8_MAX);
            au8ResponseData[1] = (uint8_t)ducar_can_version_to_number(
                app_param_get_package_t_version(), UINT8_MAX);
            au8ResponseData[2] = (uint8_t)ducar_can_version_to_number(
                app_param_get_hw_version(), UINT8_MAX);
            au8ResponseData[3] = APP_PARAM_CAN_PROTOCOL_VERSION;
            /* F200固定按8字节单帧返回。 */
            u8ResponseLength = DUCAR_CAN_DATA_DLC;
            break;

        case DUCAR_CAN_PGN_F201:
            /* 读取软件料号，料号可能超过8字节，因此允许TP发送。 */
            pszParameter = app_param_get_software_pn();
            /* 计算字符串长度，空指针按长度0处理。 */
            uParameterLength = (pszParameter != 0) ? strlen(pszParameter) : 0U;
            /* 料号为空或超过TP缓存容量时无法响应。 */
            if ((uParameterLength == 0U) ||
                (uParameterLength > DUCAR_CAN_TP_MAX_DATA_LEN))
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }
            /* 将料号文本复制到协议响应缓存。 */
            memcpy(au8ResponseData, pszParameter, uParameterLength);
            /* 保存实际响应长度，后续由单帧/TP发送函数决定传输方式。 */
            u8ResponseLength = (uint8_t)uParameterLength;
            break;

        case DUCAR_CAN_PGN_F208:
            /* 读取制造序列号。 */
            pszParameter = app_param_get_manufacture_sn();
            /* 计算序列号长度。 */
            uParameterLength = (pszParameter != 0) ? strlen(pszParameter) : 0U;
            /* Data0还要占用1字节长度，因此正文最多剩余TP缓存长度减1。 */
            if (uParameterLength > (DUCAR_CAN_TP_MAX_DATA_LEN - 1U))
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }
            /* Data0填写序列号正文长度。 */
            au8ResponseData[0] = (uint8_t)uParameterLength;
            if (uParameterLength > 0U)
            {
                /* Data1开始复制序列号正文。 */
                memcpy(&au8ResponseData[1], pszParameter, uParameterLength);
            }
            /* 响应总长度等于长度字段1字节加序列号正文长度。 */
            u8ResponseLength = (uint8_t)(uParameterLength + 1U);
            break;

        case DUCAR_CAN_PGN_F230:
            /* F230按A1.1支持读写；读取方向返回当前SN的ASCII原文，不附加长度字节。 */
            pszParameter = app_param_get_manufacture_sn();
            uParameterLength = (pszParameter != 0) ? strlen(pszParameter) : 0U;
            if (uParameterLength > APP_BMS_MANUFACTURE_SN_MAX_LEN)
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }

            if (uParameterLength == 0U)
            {
                /* 清除SN后以DLC=0的F230响应表示当前字符串为空。 */
                if (!d_can_send(&can0,
                                ducar_can_id_encode((uint8_t)(u16RequestedPgn >> 8U),
                                                    (uint8_t)u16RequestedPgn,
                                                    DUCAR_CAN_BMS_A_ADDRESS),
                                D_CAN_ID_EXTENDED,
                                au8ResponseData,
                                0U))
                {
                    (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                              ptCanIdFields->u8Sa,
                                              u16RequestedPgn,
                                              DUCAR_CAN_ACTION_NONE);
                }
                return;
            }

            memcpy(au8ResponseData, pszParameter, uParameterLength);
            u8ResponseLength = (uint8_t)uParameterLength;
            break;

        case DUCAR_CAN_PGN_F205:
            /* F205制造商名称来自项目宏，字符串按实际ASCII长度返回。 */
            pszParameter = APP_PARAM_MANUFACTURER_NAME;
            uParameterLength = strlen(pszParameter);
            if (uParameterLength > 8U)
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }
            memcpy(au8ResponseData, pszParameter, uParameterLength);
            u8ResponseLength = (uint8_t)uParameterLength;
            break;

        case DUCAR_CAN_PGN_F206:
            /* F206软件名称复用电池/平台型号参数，长度不超过协议定义的16字节。 */
            pszParameter = app_param_get_bat_type();
            uParameterLength = (pszParameter != 0) ? strlen(pszParameter) : 0U;
            if ((uParameterLength == 0U) || (uParameterLength > 16U))
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }
            memcpy(au8ResponseData, pszParameter, uParameterLength);
            u8ResponseLength = (uint8_t)uParameterLength;
            break;

        case DUCAR_CAN_PGN_F20B:
        {
            uint32_t rated_voltage_0p1v = (uint32_t)APP_PARAM_BAT_VOLTAGE / 100UL;
            uint32_t rated_capacity_0p1ah = (uint32_t)APP_PARAM_BAT_CAPACITY / 100UL;

            /* A0.6 F20B系数0.1V/0.1Ah；参数单位mV/mAh，不能精确编码则NAK。 */
            if ((((uint32_t)APP_PARAM_BAT_VOLTAGE % 100UL) != 0UL) ||
                (((uint32_t)APP_PARAM_BAT_CAPACITY % 100UL) != 0UL) ||
                (rated_voltage_0p1v > 0xFFFFUL) || (rated_capacity_0p1ah > 0xFFFFUL))
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa,
                                          u16RequestedPgn,
                                          DUCAR_CAN_ACTION_NONE);
                return;
            }

            memset(au8ResponseData, 0xFF, DUCAR_CAN_DATA_DLC);
            ducar_can_write_u16_le(au8ResponseData, 0U, (uint16_t)rated_voltage_0p1v);
            ducar_can_write_u16_le(au8ResponseData, 2U, (uint16_t)rated_capacity_0p1ah);
            u8ResponseLength = DUCAR_CAN_DATA_DLC;
            break;
        }

        case DUCAR_CAN_PGN_F229:
            /* 未校时仍回复RTC当前日历；硬件日历不可读时使用默认时间。 */
            if (!d_rtc_read(&tRtcTime) && !d_rtc_read_calendar(&tRtcTime))
            {
                memset(&tRtcTime, 0, sizeof(tRtcTime));
                tRtcTime.year = 2000U;
                tRtcTime.month = 1U;
                tRtcTime.day = 1U;
            }
            /* RTC响应未使用字节填FF。 */
            memset(au8ResponseData, 0xFF, DUCAR_CAN_DATA_DLC);
            /* Data0~Data5依次为年、月、日、时、分、秒。 */
            au8ResponseData[0] = (uint8_t)(tRtcTime.year - 2000U);
            au8ResponseData[1] = tRtcTime.month;
            au8ResponseData[2] = tRtcTime.day;
            au8ResponseData[3] = tRtcTime.hour;
            au8ResponseData[4] = tRtcTime.min;
            au8ResponseData[5] = tRtcTime.sec;
            /* RTC按8字节单帧返回。 */
            u8ResponseLength = DUCAR_CAN_DATA_DLC;
            break;

        case DUCAR_CAN_PGN_F22F:
            /* 通用查询入口已刷新发送窗口；LifeValue按8字节返回，其余字节填FF。 */
            memset(au8ResponseData, 0xFF, DUCAR_CAN_DATA_DLC);
            /* Data0返回当前生命值计数。 */
            au8ResponseData[0] = u8CanLifeValue;
            u8ResponseLength = DUCAR_CAN_DATA_DLC;
            break;

        case DUCAR_CAN_PGN_F231:
        {
            uint32_t k_ppm;
            int32_t b_ma;
            uint32_t b_raw;
            if (!app_current_calib_get(&k_ppm, &b_ma))
            {
                (void)ducar_can_queue_ack(DUCAR_CAN_NAK, ptCanIdFields->u8Sa,
                                          u16RequestedPgn, DUCAR_CAN_ACTION_NONE);
                return;
            }
            b_raw = (uint32_t)b_ma;
            au8ResponseData[0] = (uint8_t)k_ppm;
            au8ResponseData[1] = (uint8_t)(k_ppm >> 8U);
            au8ResponseData[2] = (uint8_t)(k_ppm >> 16U);
            au8ResponseData[3] = (uint8_t)(k_ppm >> 24U);
            au8ResponseData[4] = (uint8_t)b_raw;
            au8ResponseData[5] = (uint8_t)(b_raw >> 8U);
            au8ResponseData[6] = (uint8_t)(b_raw >> 16U);
            au8ResponseData[7] = (uint8_t)(b_raw >> 24U);
            u8ResponseLength = 8U;
            break;
        }

        case DUCAR_CAN_PGN_F203:
        case DUCAR_CAN_PGN_F20C:
        case DUCAR_CAN_PGN_F22E:
            /* 这些PGN当前没有业务数据源，不能返回默认数据冒充有效配置。 */
            (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                      ptCanIdFields->u8Sa,
                                      u16RequestedPgn,
                                      DUCAR_CAN_ACTION_NONE);
            return;

        default:
            /* 未知PGN不进入发送流程，直接告知主机不支持。 */
            (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                      ptCanIdFields->u8Sa,
                                      u16RequestedPgn,
                                      DUCAR_CAN_ACTION_NONE);
            return;
    }

    /* 统一走单帧或TP发送函数；发送资源不可用时返回NAK。 */
    if (!ducar_can_send_requested_data(ptCanIdFields->u8Sa,
                                       u16RequestedPgn,
                                       au8ResponseData,
                                       u8ResponseLength))
    {
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16RequestedPgn,
                                  DUCAR_CAN_ACTION_NONE);
    }
}

/*
 * 处理普通协议TP连接管理报文。
 * 只接受当前会话对应PGN的CTS、EOM ACK和ABORT，避免其他会话误推进发送状态。
 */
static void ducar_can_receive_tp_cm(const t_can_packet *ptCanPacket,
                                    const ducar_can_id_fields_t *ptCanIdFields)
{
    uint32_t u32TransportPgn;
    uint8_t u8RequestedPackets;
    uint8_t u8FirstPacket;
    uint8_t u8PacketsLeft;

    /* TP连接管理固定为8字节。 */
    if ((ptCanPacket->u8len != DUCAR_CAN_DATA_DLC) ||
        /* CTS/EOM ACK/ABORT只能由PC发送。 */
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        /* 普通协议TP只接受发往本BMS的控制帧。 */
        (ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS))
    {
        return;
    }

    /* Data5~Data7携带当前TP会话的PGN，按小端24位字段组合。 */
    u32TransportPgn = (uint32_t)ptCanPacket->au8data[5] |
                      ((uint32_t)ptCanPacket->au8data[6] << 8U) |
                      ((uint32_t)ptCanPacket->au8data[7] << 16U);
    /* PC上传SN：RTS声明实际长度，BMS按该长度发CTS。 */
    if ((ptCanPacket->au8data[0] == DUCAR_CAN_TP_RTS) &&
        (u32TransportPgn == DUCAR_CAN_PGN_F230))
    {
        uint8_t cm[8] = {DUCAR_CAN_TP_CTS, 0U, 1U, 0xFFU, 0xFFU,
                         0x30U, 0xF2U, 0U};
        uint16_t length = (uint16_t)ptCanPacket->au8data[1] |
                          ((uint16_t)ptCanPacket->au8data[2] << 8U);
        uint8_t packets = (uint8_t)((length + 6U) / 7U);
        if ((length == 0U) || (length > APP_BMS_MANUFACTURE_SN_MAX_LEN) ||
            (packets != ptCanPacket->au8data[3]) ||
            (u8CanTpRxNext != 0U))
        {
            (void)ducar_can_queue_ack(DUCAR_CAN_NAK, ptCanIdFields->u8Sa,
                                      DUCAR_CAN_PGN_F230, DUCAR_CAN_ACTION_NONE);
            return;
        }
        cm[1] = packets;
        if (!d_can_send(&can0, ducar_can_id_encode(DUCAR_CAN_PF_TP_CM,
                         ptCanIdFields->u8Sa, DUCAR_CAN_BMS_A_ADDRESS),
                         D_CAN_ID_EXTENDED, cm, sizeof(cm)))
        {
            return;
        }
        u8CanTpRxLength = (uint8_t)length;
        u8CanTpRxPackets = packets;
        u8CanTpRxNext = 1U;
        u16CanTpRxWaitTicks = 0U;
        return;
    }
    if ((ptCanPacket->au8data[0] == DUCAR_CAN_TP_ABORT) &&
        (u32TransportPgn == DUCAR_CAN_PGN_F230))
    {
        u8CanTpRxNext = 0U;
        return;
    }
    /* 其他PGN的CTS/EOM ACK不能影响当前会话。 */
    if (u32TransportPgn != (uint32_t)u16CanTpPgn)
    {
        return;
    }

    /* ABORT无论当前处于哪个等待状态，都立即释放TP会话。 */
    if (ptCanPacket->au8data[0] == DUCAR_CAN_TP_ABORT)
    {
        /* 清除TP状态，等待下一次查询响应。 */
        eCanTpState = DUCAR_CAN_TP_IDLE;
        u16CanTpWaitTicks = 0U;
        return;
    }

    /* 只有WAIT_EOM阶段才接受EOM ACK。 */
    if ((ptCanPacket->au8data[0] == DUCAR_CAN_TP_EOM_ACK) &&
        (eCanTpState == DUCAR_CAN_TP_WAIT_EOM) &&
        (ptCanPacket->au8data[1] == u8CanTpDataLength) &&
        (ptCanPacket->au8data[2] == 0U) &&
        (ptCanPacket->au8data[3] == u8CanTpPacketCount))
    {
        /* 校验长度、保留字节和包数均匹配后结束会话。 */
        eCanTpState = DUCAR_CAN_TP_IDLE;
        u16CanTpWaitTicks = 0U;
        if ((u16CanTpPgn == DUCAR_CAN_PGN_F232) && (u16CanLogNext != 0U))
        {
            if (u16CanLogNext < u16CanLogRequested)
            {
                /* 当前记录确认完成后，继续发送下一条请求的LOG。 */
                u16CanLogNext++;
                ducar_can_log_next();
            }
            else
            {
                /* 最后一条LOG收到EOM ACK后，按协议回E8成功确认F232读取完成。 */
                u16CanLogNext = 0U;
                u16CanLogRequested = 0U;
                (void)ducar_can_queue_ack(DUCAR_CAN_ACK,
                                          ptCanIdFields->u8Sa,
                                          DUCAR_CAN_PGN_F232,
                                          DUCAR_CAN_ACTION_NONE);
            }
        }
        return;
    }

    /* 既不是CTS，或当前不是等待CTS阶段时，不推进DT发送。 */
    if ((ptCanPacket->au8data[0] != DUCAR_CAN_TP_CTS) ||
        (eCanTpState != DUCAR_CAN_TP_WAIT_CTS))
    {
        return;
    }

    /* Data1表示本次允许发送的DT数量。 */
    u8RequestedPackets = ptCanPacket->au8data[1];
    /* Data2表示本次发送窗口的起始DT序号。 */
    u8FirstPacket = ptCanPacket->au8data[2];
    /* 计算从起始序号到末尾还剩多少包。 */
    /* 窗口数量、起始序号和序号连续性必须全部有效。 */
    if ((u8RequestedPackets == 0U) ||
        (u8FirstPacket == 0U) ||
        (u8FirstPacket != u8CanTpNextPacket) ||
        (u8FirstPacket > u8CanTpPacketCount))
    {
        return;
    }

    /* 按对端指定的起始序号开始本次发送窗口。 */
    u8CanTpNextPacket = u8FirstPacket;
    /* 实际窗口大小不能超过剩余包数。 */
    u8PacketsLeft = (uint8_t)(u8CanTpPacketCount - u8FirstPacket + 1U);
    u8CanTpBurstPackets = (u8RequestedPackets < u8PacketsLeft) ?
                          u8RequestedPackets : u8PacketsLeft;
    /* 收到CTS后清除等待计时。 */
    u16CanTpWaitTicks = 0U;
    /* 下一次发送任务开始发送DT。 */
    eCanTpState = DUCAR_CAN_TP_SEND_DT;
}

/*
 * 处理普通协议TP数据报文。
 * 当前A0.1的普通查询数据由BMS发送，PC只接收响应控制，因此这里不组包。
 */
static void ducar_can_receive_tp_dt(const t_can_packet *ptCanPacket,
                                    const ducar_can_id_fields_t *ptCanIdFields)
{
    uint8_t offset, count, cm[8];
    if ((u8CanTpRxNext == 0U) || !ducar_can_id_is_pc_source(ptCanIdFields) ||
        (ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS) ||
        (ptCanPacket->u8len < 2U) ||
        (ptCanPacket->au8data[0] != u8CanTpRxNext))
    {
        return;
    }
    offset = (uint8_t)((u8CanTpRxNext - 1U) * 7U);
    count = (uint8_t)(u8CanTpRxLength - offset);
    if (count > 7U) count = 7U;
    if (ptCanPacket->u8len < (uint8_t)(count + 1U)) return;
    memcpy(&au8CanTpRxSn[offset], &ptCanPacket->au8data[1], count);
    u16CanTpRxWaitTicks = 0U;
    if (u8CanTpRxNext++ < u8CanTpRxPackets) return;
    u8CanTpRxNext = 0U;
    memset(cm, 0xFF, sizeof(cm));
    cm[0] = DUCAR_CAN_TP_EOM_ACK;
    cm[1] = u8CanTpRxLength;
    cm[2] = 0U;
    cm[3] = u8CanTpRxPackets;
    cm[5] = 0x30U;
    cm[6] = 0xF2U;
    cm[7] = 0U;
    (void)d_can_send(&can0, ducar_can_id_encode(DUCAR_CAN_PF_TP_CM,
                     ptCanIdFields->u8Sa, DUCAR_CAN_BMS_A_ADDRESS),
                     D_CAN_ID_EXTENDED, cm, sizeof(cm));
    (void)ducar_can_queue_ack(app_param_set_manufacture_sn(au8CanTpRxSn,
                              u8CanTpRxLength) ? DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                              ptCanIdFields->u8Sa, DUCAR_CAN_PGN_F230,
                              DUCAR_CAN_ACTION_NONE);
}

/*
 * 处理PF=08基础控制：Data0=0时由Data1设置独立产品模式；
 * Data0=1/2仍为复位/关机，ACK发送完成后才执行动作。
 */
static void ducar_can_receive_reset_command(const t_can_packet *ptCanPacket,
                                            const ducar_can_id_fields_t *ptCanIdFields)
{
    uint16_t u16FunctionCode = (uint16_t)((uint16_t)DUCAR_CAN_PF_RESET << 8U);
    ducar_can_action_t eRequestedAction = DUCAR_CAN_ACTION_NONE;
    uint8_t u8Command;

    /* 复位/关机控制必须是完整8字节PC点对点扩展帧。 */
    if ((ptCanPacket->u8len != DUCAR_CAN_DATA_DLC) ||
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        (ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS) ||
        (u8CanAckPending != 0U) ||
        (eCanActionWaitingTx != DUCAR_CAN_ACTION_NONE))
    {
        return;
    }

    /* Data0是控制命令：0设置模式、1复位、2关机。 */
    u8Command = ptCanPacket->au8data[0];
    if (u8Command == 0x00U)
    {
        uint8_t u8Mode = ptCanPacket->au8data[1];
        /* 旧版Data1=FF的空命令保持无操作；其他值只允许0或1。 */
        uint8_t u8Result = (u8Mode == 0xFFU) || app_param_set_product_mode(u8Mode);
        (void)ducar_can_queue_ack(u8Result ? DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16FunctionCode,
                                  DUCAR_CAN_ACTION_NONE);
        return;
    }
    else if (u8Command == 0x01U)
    {
        /* 记录ACK发送完成后执行复位。 */
        eRequestedAction = DUCAR_CAN_ACTION_RESET;
    }
    else if (u8Command == 0x02U)
    {
        /* 记录ACK发送完成后执行关机。 */
        eRequestedAction = DUCAR_CAN_ACTION_POWER_OFF;
    }
    else
    {
        /* 未定义命令不执行任何硬件动作，回复NAK。 */
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16FunctionCode,
                                  DUCAR_CAN_ACTION_NONE);
        return;
    }

    /* 先保存关机保护状态；保存失败时不能继续执行复位/关机。 */
    if (((eRequestedAction == DUCAR_CAN_ACTION_RESET) && !app_bms_soc_save_for_transition()) ||
        (app_bms_fault_save_shutdown_pf() == 0U))
    {
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16FunctionCode,
                                  DUCAR_CAN_ACTION_NONE);
        return;
    }

    /* 业务允许后，只排队ACK和对应的后续动作。 */
    (void)ducar_can_queue_ack(DUCAR_CAN_ACK,
                              ptCanIdFields->u8Sa,
                              u16FunctionCode,
                              eRequestedAction);
}

/*
 * 处理PF=09 MOS远程控制（V0.8）。
 * Data0选择充电/放电/预放MOS，Data1指定打开或关闭，Data2为命令保持时间（分钟）。
 * 远程命令覆盖软件保护禁止，但仍受数据、DETD、唤醒采样、预放流程及关机强制关断约束。
 */
static void ducar_can_receive_mos_command(const t_can_packet *ptCanPacket,
                                          const ducar_can_id_fields_t *ptCanIdFields)
{
    uint8_t u8MosSelect;
    uint8_t u8MosState;
    uint8_t u8DurationMin;
    uint16_t u16FunctionCode = (uint16_t)((uint16_t)DUCAR_CAN_PF_MOS_CONTROL << 8U);
    app_mos_cmd_t eMosCommand;
    app_result_t eCommandResult;

    /* MOS控制同样要求完整8字节PC点对点扩展帧。 */
    if ((ptCanPacket->u8len != DUCAR_CAN_DATA_DLC) ||
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        (ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS) ||
        (u8CanAckPending != 0U) ||
        (eCanActionWaitingTx != DUCAR_CAN_ACTION_NONE))
    {
        return;
    }

    /* Data0选择目标MOS。 */
    u8MosSelect = ptCanPacket->au8data[0];
    /* Data1表示目标MOS状态。 */
    u8MosState = ptCanPacket->au8data[1];
    /* Data2是1字节分钟数；0按协议约定的默认1分钟处理。 */
    u8DurationMin = ptCanPacket->au8data[2];
    if (u8MosState == 0x00U)
    {
        /* 0表示关闭MOS。 */
        eMosCommand = APP_MOS_OFF;
    }
    else if (u8MosState == 0x01U)
    {
        /* 1表示打开MOS。 */
        eMosCommand = APP_MOS_ON;
    }
    else
    {
        /* 其他状态值不接受，避免把非法值传入电源管理模块。 */
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16FunctionCode,
                                  DUCAR_CAN_ACTION_NONE);
        return;
    }

    /* 交给电源管理模块执行具体MOS命令。 */
    eCommandResult = app_h_power_manage_set_remote_mos_command(u8MosSelect,
                                                                eMosCommand,
                                                                u8DurationMin);
    /* 根据业务层执行结果返回ACK或NAK。 */
    (void)ducar_can_queue_ack((eCommandResult == APP_RESULT_OK) ?
                              DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                              ptCanIdFields->u8Sa,
                              u16FunctionCode,
                              DUCAR_CAN_ACTION_NONE);
}

/*
 * 处理当前工程暂未实现的控制PF。
 * 报文格式和来源正确时明确回复NAK，但不修改硬件、业务状态或总快照。
 */
static void ducar_can_receive_unsupported_control(
    const t_can_packet *ptCanPacket,
    const ducar_can_id_fields_t *ptCanIdFields)
{
    /* 用收到的PF组成被确认功能码，便于上位机定位不支持的命令。 */
    uint16_t u16FunctionCode = (uint16_t)((uint16_t)ptCanIdFields->u8Pf << 8U);

    /* 只对完整、来源正确、目标正确的控制帧回复NAK。 */
    if ((ptCanPacket->u8len == DUCAR_CAN_DATA_DLC) &&
        ducar_can_id_is_pc_source(ptCanIdFields) &&
        (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
    {
        /* A00低功耗阈值及B00/C00调试注入当前没有对应业务接口，不写硬件或快照。 */
        (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                  ptCanIdFields->u8Sa,
                                  u16FunctionCode,
                                  DUCAR_CAN_ACTION_NONE);
    }
}

/*
 * 处理总线静默控制。
 * 该命令只改变CAN发送许可标志，协议规定本命令不返回ACK/NAK。
 */
static void ducar_can_receive_bus_control(
    const t_can_packet *ptCanPacket,
    const ducar_can_id_fields_t *ptCanIdFields)
{
    /* 总线静默命令必须是完整8字节PC报文。 */
    if ((ptCanPacket->u8len != DUCAR_CAN_DATA_DLC) ||
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        ((ptCanIdFields->u8Ps != DUCAR_CAN_GLOBAL_ADDRESS) &&
         (ptCanIdFields->u8Ps != DUCAR_CAN_BMS_A_ADDRESS)))
    {
        return;
    }

    /* Data0=1表示进入静默状态。 */
    if (ptCanPacket->au8data[0] == 0x01U)
    {
        u8CanBusSilent = 1U;
    }
    /* Data0=2表示恢复总线发送。 */
    else if (ptCanPacket->au8data[0] == 0x02U)
    {
        u8CanBusSilent = 0U;
    }
    /* A0.1规定总线静默控制不回复ACK/NAK。 */
}

/*
 * 处理PF=F2中的RTC写入命令，将Data0~Data5转换为年月日时分秒后写入RTC，
 * 写入结果通过对应PGN的ACK/NAK回复。
 */
static void ducar_can_receive_rtc_write(const t_can_packet *ptCanPacket,
                                        const ducar_can_id_fields_t *ptCanIdFields)
{
    d_rtc_time_t tRtcTime;
    uint8_t u8WriteSucceeded = 0U;

    /* RTC写入要求完整8字节PC请求，且PS使用F229。 */
    if ((ptCanPacket->u8len != DUCAR_CAN_DATA_DLC) ||
        (!ducar_can_id_is_pc_source(ptCanIdFields)) ||
        (ptCanIdFields->u8Ps != (uint8_t)DUCAR_CAN_PGN_F229))
    {
        return;
    }

    /* Data0为相对2000年的年份。 */
    tRtcTime.year = (uint16_t)(2000U + ptCanPacket->au8data[0]);
    /* Data1为月份。 */
    tRtcTime.month = ptCanPacket->au8data[1];
    /* Data2为日期。 */
    tRtcTime.day = ptCanPacket->au8data[2];
    /* Data3为小时。 */
    tRtcTime.hour = ptCanPacket->au8data[3];
    /* Data4为分钟。 */
    tRtcTime.min = ptCanPacket->au8data[4];
    /* Data5为秒。 */
    tRtcTime.sec = ptCanPacket->au8data[5];
    /* 当前协议未传星期，写0表示不更新/不使用该字段。 */
    tRtcTime.week = 0U;
    /* 调用底层RTC写入接口，并将布尔结果转换为协议状态。 */
    u8WriteSucceeded = d_rtc_write(&tRtcTime) ? 1U : 0U;

    /* 写入成功回复ACK，失败回复NAK。 */
    (void)ducar_can_queue_ack(u8WriteSucceeded != 0U ? DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                              ptCanIdFields->u8Sa,
                              DUCAR_CAN_PGN_F229,
                              DUCAR_CAN_ACTION_NONE);
}

/*
 * 普通CAN报文的PF一级分发入口。
 * 先按PF区分查询、TP、控制和RTC，再进入各自的独立处理函数。
 */
static void ducar_can_receive_normal_pf(const t_can_packet *ptCanPacket,
                                        const ducar_can_id_fields_t *ptCanIdFields)
{
    if (app_current_calib_cal2_is_active() &&
        (ptCanIdFields->u8Pf != DUCAR_CAN_PF_REQUEST) &&
        (ptCanIdFields->u8Pf != DUCAR_CAN_PF_CURRENT_CALIB) &&
        (ptCanIdFields->u8Pf != DUCAR_CAN_PF_ACK))
    {
        /* 活跃校准期间拒绝其他写操作和TP接收，不改变保护或MOS。 */
        return;
    }
    /* 与GD7627一致：先看PF，再进入该报文类别的独立处理函数。 */
    /* PF决定后续解释方式，不能在这里直接按数据内容猜测协议。 */
    switch (ptCanIdFields->u8Pf)
    {
        case DUCAR_CAN_PF_REQUEST:
            /* EA请求进入查询PGN处理。 */
            ducar_can_receive_request(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_TP_CM:
            /* EC连接管理帧进入普通TP发送会话处理。 */
            ducar_can_receive_tp_cm(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_TP_DT:
            /* EB数据帧预留给普通TP接收处理。 */
            ducar_can_receive_tp_dt(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_ACK:
            /* ACK属于对端对BMS控制/IAP响应的确认；当前普通协议无后续动作。 */
            break;

        case DUCAR_CAN_PF_RESET:
            /* 08控制帧进入复位/关机命令处理。 */
            ducar_can_receive_reset_command(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_MOS_CONTROL:
            /* 09控制帧进入MOS控制处理。 */
            ducar_can_receive_mos_command(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_STANDBY_CURRENT:
        case DUCAR_CAN_PF_DEBUG_V:
        case DUCAR_CAN_PF_DEBUG_CELL:
            /* A/B/C类当前未实现，统一走不支持控制处理。 */
            ducar_can_receive_unsupported_control(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_CAN_CONTROL:
            /* E0控制帧进入总线静默控制处理。 */
            ducar_can_receive_bus_control(ptCanPacket, ptCanIdFields);
            break;

        case DUCAR_CAN_PF_CURRENT_CALIB:
            /* E700按DLC严格区分旧单点与CAL2。 */
            if (ducar_can_id_is_pc_source(ptCanIdFields) &&
                (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
            {
                uint32_t raw = (uint32_t)ptCanPacket->au8data[0] |
                               ((uint32_t)ptCanPacket->au8data[1] << 8U) |
                               ((uint32_t)ptCanPacket->au8data[2] << 16U) |
                               ((uint32_t)ptCanPacket->au8data[3] << 24U);
                if (ptCanPacket->u8len == 4U)
                {
                    (void)ducar_can_queue_ack(app_current_calib_submit((int32_t)raw) ?
                                              DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                                              ptCanIdFields->u8Sa, 0xE700U,
                                              DUCAR_CAN_ACTION_NONE);
                }
                else if (ptCanPacket->u8len == 8U)
                {
                    uint16_t session = (uint16_t)ptCanPacket->au8data[6] |
                                       ((uint16_t)ptCanPacket->au8data[7] << 8U);
                    uint8_t status;
                    if (s_cal2_reply_count >= DUCAR_CAL2_REPLY_CAPACITY) break;
                    if ((ptCanPacket->au8data[4] == 1U) &&
                        (ducar_iap_is_busy() || ducar_can_iap_protocol_is_busy() ||
                         (eCanTpState != DUCAR_CAN_TP_IDLE) ||
                         (u16CanLogNext != 0U)))
                    {
                        status = 2U;
                    }
                    else
                    {
                        status = app_current_calib_cal2_request(ptCanPacket->au8data[4],
                                     ptCanPacket->au8data[5], session,
                                     (int32_t)raw, ptCanIdFields->u8Sa);
                    }
                    (void)ducar_can_cal2_queue_reply(ptCanIdFields->u8Sa, status,
                                                     ptCanPacket->au8data[4],
                                                     ptCanPacket->au8data[5], session);
                }
                else
                {
                    (void)ducar_can_queue_ack(DUCAR_CAN_NAK, ptCanIdFields->u8Sa,
                                              0xE700U, DUCAR_CAN_ACTION_NONE);
                }
            }
            break;

        case DUCAR_CAN_PF_SOC_WRITE:
            /* E200：Data0为主机请求SOC，U8整数百分比；其余字节保留并忽略。 */
            if (ducar_can_id_is_pc_source(ptCanIdFields) &&
                (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
            {
                uint8_t requested_soc = 0U;
                uint8_t accepted = 0U;

                if ((ptCanPacket->u8len >= 1U) &&
                    (u8CanSocOcvAckPending == 0U) &&
                    (u8CanSocWriteAckPending == 0U))
                {
                    requested_soc = ptCanPacket->au8data[0];
                    if ((requested_soc <= 100U) &&
                        app_bms_soc_write_request(requested_soc))
                    {
                        u8CanSocWriteAckPending = 1U;
                        u8CanSocWriteAckDestination = ptCanIdFields->u8Sa;
                        accepted = 1U;
                    }
                }

                /* 无法排队、长度错误或SOC越界时立即回NAK，不改动SOC状态。 */
                if (accepted == 0U)
                {
                    (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                              ptCanIdFields->u8Sa,
                                              DUCAR_CAN_PGN_E200,
                                              DUCAR_CAN_ACTION_NONE);
                }
            }
            break;

        case DUCAR_CAN_PF_CLEAR_SN:
            /* E300：共享参数页读改写，仅清除SN，产品模式保持不变。 */
            if (ducar_can_id_is_pc_source(ptCanIdFields) &&
                (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
            {
                uint8_t ok = ((ptCanPacket->u8len >= 1U) &&
                              (ptCanPacket->au8data[0] == 0x01U)) ?
                             (app_param_clear_manufacture_sn() ? 1U : 0U) : 0U;
                (void)ducar_can_queue_ack(ok ? DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa, 0xE300U,
                                          DUCAR_CAN_ACTION_NONE);
            }
            break;

        case 0xE6U:
            /* E600：清AFE、PF锁存及软件故障，三步全部成功才ACK。 */
            if (ducar_can_id_is_pc_source(ptCanIdFields) &&
                (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
            {
                uint8_t ok = 0U;
                if ((ptCanPacket->u8len >= 1U) &&
                    (ptCanPacket->au8data[0] == 0U) &&
                    (app_hw_apply_clear_all_afe_fault() != 0U) &&
                    (app_bms_fault_clear_shutdown_pf() != 0U))
                {
                    app_bms_fault_clear_all();
                    ok = 1U;
                }
                (void)ducar_can_queue_ack(ok ? DUCAR_CAN_ACK : DUCAR_CAN_NAK,
                                          ptCanIdFields->u8Sa, 0xE600U,
                                          DUCAR_CAN_ACTION_NONE);
            }
            break;

        case 0xE9U:
            /* E900：Data0=1时让下一次BMS SOC周期执行OCV重算。 */
            if (ducar_can_id_is_pc_source(ptCanIdFields) &&
                (ptCanIdFields->u8Ps == DUCAR_CAN_BMS_A_ADDRESS))
            {
                app_bms_snapshot_t snapshot;
                uint8_t ok = ((ptCanPacket->u8len >= 1U) &&
                              (ptCanPacket->au8data[0] == 1U) &&
                              (u8CanSocOcvAckPending == 0U) &&
                              (u8CanSocWriteAckPending == 0U) &&
                              (app_bms_soc_write_status() == APP_BMS_SOC_WRITE_IDLE)) ?
                              1U : 0U;
                app_bms_snapshot_read(&snapshot);
                if ((ok != 0U) && (snapshot.data_ready != 0U) &&
                    (snapshot.max_cell_mv >= 2000U) &&
                    app_bms_force_soc_ocv())
                {
                    u8CanSocOcvAckPending = 1U;
                    u8CanSocOcvAckDestination = ptCanIdFields->u8Sa;
                    u16CanSocOcvWaitTicks = 0U;
                }
                else
                {
                    (void)ducar_can_queue_ack(DUCAR_CAN_NAK,
                                              ptCanIdFields->u8Sa, 0xE900U,
                                              DUCAR_CAN_ACTION_NONE);
                }
            }
            break;

        case DUCAR_CAN_PF_F2:
            /* F2功能帧当前用于RTC写入。 */
            ducar_can_receive_rtc_write(ptCanPacket, ptCanIdFields);
            break;

        default:
            /* PF=E1由app_can分流至IAP；其他PF不属于当前普通协议。 */
            break;
    }
}

/*
 * 普通协议接收总入口：完成扩展帧/长度检查、CAN ID解析，再交给PF分发。
 * APP层的E1 CAN IAP报文由上层app_can.c优先分流，不在这里处理。
 */
void ducar_can_protocol_receive(const t_can_packet *ptCanPacket)
{
    ducar_can_id_fields_t tCanIdFields;

    /* 空指针、非扩展帧和超长数据帧不进入普通协议。 */
    if ((ptCanPacket == 0) ||
        (ptCanPacket->u32idType != D_CAN_ID_EXTENDED) ||
        (ptCanPacket->u8len > DUCAR_CAN_MAX_DATA_LEN))
    {
        return;
    }

    /* 将29位CAN ID拆成PF/PS/SA等字段。 */
    ducar_can_id_decode(ptCanPacket->u32id, &tCanIdFields);
    /* 由PF一级分发到具体普通协议处理函数。 */
    ducar_can_receive_normal_pf(ptCanPacket, &tCanIdFields);
}

/*
 * 普通协议TP发送状态机。
 * 状态顺序为SEND_RTS -> WAIT_CTS -> SEND_DT -> WAIT_EOM；
 * 只有收到对端CTS后才按窗口发送DT，最后等待EOM ACK结束会话。
 */
static void ducar_can_tp_transmit_task(void)
{
    uint8_t au8CanData[DUCAR_CAN_DATA_DLC];
    uint8_t u8DataOffset;
    uint8_t u8RemainingData;
    uint8_t u8DataByteCount;

    /* SEND_RTS状态只发送一次RTS，发送成功后等待对端CTS。 */
    if (eCanTpState == DUCAR_CAN_TP_SEND_RTS)
    {
        /* RTS未使用字段先全部填FF。 */
        memset(au8CanData, 0xFF, sizeof(au8CanData));
        /* Data0=RTS。 */
        au8CanData[0] = DUCAR_CAN_TP_RTS;
        /* Data1~Data2为总数据长度，当前缓存最多60字节。 */
        au8CanData[1] = u8CanTpDataLength;
        au8CanData[2] = 0U;
        /* Data3为DT总包数。 */
        au8CanData[3] = u8CanTpPacketCount;
        /* Data4为保留值。 */
        au8CanData[4] = 0xFFU;
        /* Data5~Data7为本次传输PGN。 */
        au8CanData[5] = (uint8_t)(u16CanTpPgn & 0xFFU);
        au8CanData[6] = (uint8_t)(u16CanTpPgn >> 8U);
        au8CanData[7] = 0U;

        /* RTS发送成功后才能进入等待CTS，发送失败则保留SEND_RTS重试。 */
        if (d_can_send(&can0,
                       ducar_can_id_encode(DUCAR_CAN_PF_TP_CM,
                                           u8CanTpDestination,
                                           DUCAR_CAN_BMS_A_ADDRESS),
                       D_CAN_ID_EXTENDED,
                       au8CanData,
                       DUCAR_CAN_DATA_DLC))
        {
            u16CanTpWaitTicks = 0U;
            eCanTpState = DUCAR_CAN_TP_WAIT_CTS;
        }
        return;
    }

    /* 只有SEND_DT且窗口有剩余配额时才发送数据包。 */
    if ((eCanTpState != DUCAR_CAN_TP_SEND_DT) ||
        (u8CanTpBurstPackets == 0U) ||
        (u8CanTpNextPacket == 0U))
    {
        return;
    }

    /* DT未使用字节填FF。 */
    memset(au8CanData, 0xFF, sizeof(au8CanData));
    /* Data0为DT序号。 */
    au8CanData[0] = u8CanTpNextPacket;
    /* 根据序号计算该DT在缓存中的7字节起始偏移。 */
    u8DataOffset = (uint8_t)((u8CanTpNextPacket - 1U) * 7U);
    /* 计算当前序号之后剩余的有效数据长度。 */
    u8RemainingData = (uint8_t)(u8CanTpDataLength - u8DataOffset);
    /* 最后一包可能不足7字节，其余包固定携带7字节。 */
    u8DataByteCount = (u8RemainingData > 7U) ? 7U : u8RemainingData;
    /* 复制当前DT的有效数据。 */
    memcpy(&au8CanData[1], &au8CanTpData[u8DataOffset], u8DataByteCount);
    /* TP.DT固定DLC=8；有效长度由RTS声明，尾帧剩余字节已经填FF。 */

    /* CAN邮箱暂时不可用时，不推进序号，等待下一个任务周期重试。 */
    if (!d_can_send(&can0,
                    ducar_can_id_encode(DUCAR_CAN_PF_TP_DT,
                                        u8CanTpDestination,
                                        DUCAR_CAN_BMS_A_ADDRESS),
                    D_CAN_ID_EXTENDED,
                    au8CanData,
                    DUCAR_CAN_DATA_DLC))
    {
        return;
    }

    /* 当前DT发送成功，推进到下一个序号。 */
    u16CanTpWaitTicks = 0U;
    u8CanTpNextPacket++;
    /* 消耗本次CTS允许的一个发送配额。 */
    u8CanTpBurstPackets--;
    /* 全部DT发送完成后等待EOM ACK。 */
    if (u8CanTpNextPacket > u8CanTpPacketCount)
    {
        eCanTpState = DUCAR_CAN_TP_WAIT_EOM;
    }
    /* 当前窗口耗尽但还有数据时，回到等待下一次CTS。 */
    else if (u8CanTpBurstPackets == 0U)
    {
        eCanTpState = DUCAR_CAN_TP_WAIT_CTS;
    }

}

/*
 * 控制命令回复后的动作调度。
 * ACK/NAK本身先进入CAN发送队列；复位、关机等动作必须等发送完成或超时后执行，
 * 避免动作立即发生导致ACK还未发出就丢失。
 */
void ducar_can_control_reply_task(void)
{
    ducar_can_action_t eActionToRun;
    uint8_t soc_write_status;
    uint8_t soc_ocv_save_status;

    if (u8CanIapSilenceActive != 0U)
    {
        return;
    }

    ducar_can_cal2_reply_task();

    /* E900只在BMS任务完成OCV重算且Flash保存成功后回复ACK。 */
    if (u8CanSocOcvAckPending != 0U)
    {
        bool result_ready = false;
        uint8_t ack_status = DUCAR_CAN_NAK;

        soc_ocv_save_status = app_bms_soc_ocv_save_status();

        /* 超时结果一旦确定，即使随后OCV完成也仍返回本次超时NAK。 */
        if (u16CanSocOcvWaitTicks >= 5000U)
        {
            result_ready = true;
        }
        else if ((soc_ocv_save_status == APP_BMS_SOC_OCV_SAVED) ||
                 (soc_ocv_save_status == APP_BMS_SOC_OCV_SAVE_FAILED))
        {
            result_ready = true;
            ack_status = (soc_ocv_save_status == APP_BMS_SOC_OCV_SAVED) ?
                         DUCAR_CAN_ACK : DUCAR_CAN_NAK;
        }
        else if (++u16CanSocOcvWaitTicks >= 5000U)
        {
            result_ready = true;
        }

        /* 只有结果真正排进ACK槽后才能释放E900；忙时先发送已排队的其他ACK。 */
        if (result_ready && ducar_can_queue_ack(ack_status,
                                                u8CanSocOcvAckDestination,
                                                0xE900U, DUCAR_CAN_ACTION_NONE))
        {
            u8CanSocOcvAckPending = 0U;
            app_bms_soc_ocv_complete();
            return;
        }
    }

    /* E200只有在BMS任务完成OCV校验并写入主机请求SOC后才回ACK。 */
    if (u8CanSocWriteAckPending != 0U)
    {
        soc_write_status = app_bms_soc_write_status();
        if ((soc_write_status == APP_BMS_SOC_WRITE_SUCCESS) ||
            (soc_write_status == APP_BMS_SOC_WRITE_FAILED))
        {
            uint8_t ack_status = (soc_write_status == APP_BMS_SOC_WRITE_SUCCESS) ?
                                 DUCAR_CAN_ACK : DUCAR_CAN_NAK;
            if (ducar_can_queue_ack(ack_status,
                                    u8CanSocWriteAckDestination,
                                    DUCAR_CAN_PGN_E200,
                                    DUCAR_CAN_ACTION_NONE))
            {
                u8CanSocWriteAckPending = 0U;
                app_bms_soc_write_complete();
                return;
            }
        }
        else
        {
            return;
        }
        /* ACK槽被其他命令占用时，继续发送那个已排队的ACK，避免互相阻塞。 */
    }

    /* 如果ACK已经发出但关联动作仍在等待CAN邮箱空闲，优先处理该动作。 */
    if (eCanActionWaitingTx != DUCAR_CAN_ACTION_NONE)
    {
        /* 邮箱和软件发送队列均空闲时可以执行动作。 */
        if (d_can_is_tx_idle(&can0))
        {
            if ((eCanActionWaitingTx == DUCAR_CAN_ACTION_RESET) &&
                !app_bms_soc_save_for_transition())
            {
                return;
            }
            /* 取出待执行动作，并清除挂起标志。 */
            eActionToRun = eCanActionWaitingTx;
            eCanActionWaitingTx = DUCAR_CAN_ACTION_NONE;
        }
        /* 邮箱未空闲时累计等待时间。 */
        else if (++u8CanActionWaitTicks >= DUCAR_CAN_ACK_WAIT_MAX_TICKS)
        {
            if ((eCanActionWaitingTx == DUCAR_CAN_ACTION_RESET) &&
                !app_bms_soc_save_for_transition())
            {
                u8CanActionWaitTicks = DUCAR_CAN_ACK_WAIT_MAX_TICKS - 1U;
                return;
            }
            /* TX硬件异常时最多等100ms，不让复位/关机命令永久挂起。 */
            eActionToRun = eCanActionWaitingTx;
            eCanActionWaitingTx = DUCAR_CAN_ACTION_NONE;
        }
        else
        {
            return;
        }

        /* 无论正常完成还是超时，都清除动作等待计数。 */
        u8CanActionWaitTicks = 0U;
        if (eActionToRun == DUCAR_CAN_ACTION_RESET)
        {
            /* 执行复位动作。 */
            d_system_reset();
        }
        else if (eActionToRun == DUCAR_CAN_ACTION_POWER_OFF)
        {
            /* ACK发送完成后提交统一的软关机流程，不在协议层直接切断12V。 */
            (void)app_power_manage_request_shutdown();
        }
        return;
    }

    /* 没有等待动作时，检查是否有新的ACK/NAK需要发送。 */
    if (u8CanAckPending != 0U)
    {
        /* 尝试把缓存的ACK/NAK提交到底层CAN发送队列。 */
        if (ducar_can_send_ack(u8CanAckStatus,
                               u8CanAckDestination,
                               u16CanAckFunctionCode))
        {
            /* ACK提交成功后清除待发送标志。 */
            u8CanAckPending = 0U;
            if (eCanActionAfterAck != DUCAR_CAN_ACTION_NONE)
            {
                /* 如果有后续动作，转为等待发送完成状态。 */
                eCanActionWaitingTx = eCanActionAfterAck;
                u8CanActionWaitTicks = 0U;
            }
            /* 清除一次性动作缓存，避免下一次ACK重复执行。 */
            eCanActionAfterAck = DUCAR_CAN_ACTION_NONE;
        }
        /* ACK提交失败时累计重试次数。 */
        else if (++u8CanActionWaitTicks >= DUCAR_CAN_ACK_WAIT_MAX_TICKS)
        {
            /* ACK在100ms内无法进入邮箱时丢弃回复，不执行其关联复位/关机动作。 */
            u8CanAckPending = 0U;
            eCanActionAfterAck = DUCAR_CAN_ACTION_NONE;
            u8CanActionWaitTicks = 0U;
        }
        return;
    }

    ducar_can_tp_transmit_task();
}
