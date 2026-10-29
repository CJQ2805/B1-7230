/*
 * 宗申多电池 CAN B1 协议：只负责客户侧标准帧主动发送，不接管 DUCAR 扩展帧。
 * 调度周期为 1ms，由 Comm_Task 调用；KEY在位时持续主动发送，KEY缺失时按电流空闲超时停止。
 * 快照每10ms更新，协议始终读取一致副本。
 */
#include "Protocol/Comm/zs_b1_can_protocol.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "App/app_bms_snapshot.h"
#include "App/app_charge_time.h"
#include "App/app_h_power_manage.h"
#include "App/app_bms_status.h"
#include "App/app_param.h"
#include "Driver/d_can.h"

#define ZS_B1_CAN_DLC                 8U
/* 启用宗申B1标准帧定时主动发送。 */
#define ZS_B1_ACTIVE_TX_ENABLE        1U
#define ZS_B1_ID_BMS_FAULT            0x019U
#define ZS_B1_ID_BMS_STATUS1          0x089U
#define ZS_B1_ID_BMS_STATUS2          0x619U
#define ZS_B1_ID_BMS_CHARGE_CONTROL   0x621U
#define ZS_B1_ID_BMS_STATUS3          0x519U
#define ZS_B1_ID_BMS_STATUS4          0x5A0U
#define ZS_B1_ID_BMS_STATUS5          0x5ABU

#define ZS_B1_PERIOD_STATUS1_MS       20U
#define ZS_B1_PERIOD_100MS             100U
#define ZS_B1_PERIOD_500MS             500U
#define ZS_B1_CHARGER_STOP_DELAY_MS    10000UL
#define ZS_B1_CHARGER_STOP_TIMEOUT_MS  5000UL

typedef struct
{
    uint8_t tx_active;
    uint8_t idle_timeout_complete;
    uint8_t charge_stop_commanded;
    uint8_t charge_stop_waiting;
    uint8_t charger_stop_failed;
    uint8_t charger_stop_confirmed;
    uint8_t charger_fault_report_latched;
    uint8_t charge_result_valid;
    uint8_t charge_result_status;
    uint8_t charge_result_from_obc;
    uint8_t charge_result_from_bms;
    uint8_t charge_mos_forced_off;
    uint8_t session_idle_latched;
    uint8_t obc_frame_sequence_valid;
    uint8_t previous_detd;
    uint8_t previous_detc;
    uint8_t counter_fault;
    uint8_t counter_status1;
    uint8_t counter_charge_control;
    uint8_t next_cell_index;
    uint8_t due_100ms_mask;
    uint8_t due_500ms_mask;
    uint32_t low_current_ms;
    uint32_t charge_stop_condition_ms;
    uint32_t charge_stop_wait_ms;
    uint32_t last_obc_frame_sequence;
    uint16_t timer_status1_ms;
    uint16_t timer_100ms_ms;
    uint16_t timer_500ms_ms;
} zs_b1_can_state_t;

static zs_b1_can_state_t s_zs_b1_can;

static uint32_t zs_b1_abs_current_ma(int32_t current_ma)
{
    if (current_ma >= 0)
    {
        return (uint32_t)current_ma;
    }

    /* Avoid signed overflow for INT32_MIN. */
    return (uint32_t)(-(current_ma + 1)) + 1U;
}

static uint16_t zs_b1_limit_u16(uint32_t value)
{
    return (value > UINT16_MAX) ? UINT16_MAX : (uint16_t)value;
}

static uint16_t zs_b1_limit_u12(uint32_t value)
{
    /* 0x089电流字段上限1023A，分辨率0.25A/bit：1023 / 0.25 = 4092 raw。 */
    return (value > 4092U) ? 4092U : (uint16_t)value;
}

static uint16_t zs_b1_limit_charge_current_005a(uint32_t value)
{
    /* 0x621充电电流字段上限204A，分辨率0.05A/bit：204 / 0.05 = 4080 raw。 */
    return (value > 4080U) ? 4080U : (uint16_t)value;
}

static uint8_t zs_b1_limit_u8(uint32_t value)
{
    return (value > UINT8_MAX) ? UINT8_MAX : (uint8_t)value;
}

static void zs_b1_put_u16_le(uint8_t *data, uint8_t offset, uint16_t value)
{
    data[offset] = (uint8_t)(value & 0xFFU);
    data[offset + 1U] = (uint8_t)(value >> 8U);
}

static bool zs_b1_send(uint32_t id, const uint8_t data[ZS_B1_CAN_DLC])
{
#if (ZS_B1_ACTIVE_TX_ENABLE != 0U)
    return d_can_send(&can0, id, D_CAN_ID_STANDARD, data, ZS_B1_CAN_DLC);
#else
    (void)id;
    (void)data;
    return true;
#endif
}

static bool zs_b1_has_charge_fault(const app_bms_snapshot_t *snapshot)
{
    const t_bms_fault_flag *fault = &snapshot->fault.active.detail;
    const t_charge_fault_A *charge_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &fault->ucharge_fault_B.tcharge_fault_B;
    const t_general_fault_A *general_a = &fault->ugeneral_fault_A.tgeneral_fault_A;

    /* 满充不是故障；其余会阻止继续充电的保护/锁定作为充电故障。 */
    return ((charge_a->b1cell_pro_cov != 0U) ||
            (charge_a->b1cell_pro_cov2 != 0U) ||
            (charge_a->b1pack_pro_occ1 != 0U) ||
            (charge_a->b1pack_pro_occ2 != 0U) ||
            (charge_a->b1pack_pro_orcc != 0U) ||
            (charge_a->b1pack_pro_otc != 0U) ||
            (charge_a->b1pack_pro_utc != 0U) ||
            (charge_b->b1pack_pro_pov != 0U) ||
            (charge_b->b1pack_occ_lock != 0U) ||
            (general_a->b1NTC_fault != 0U));
}

static bool zs_b1_has_obc_stop_fault(const app_bms_snapshot_t *snapshot,
                                     uint8_t detc)
{
    /* 0x393 Data0 bit0..1=11表示充电机三级故障，需要停止充电。 */
    return ((detc != 0U) && (snapshot->obc.frame_valid != 0U) &&
            (snapshot->obc.status == APP_BMS_OBC_STATE_FAULT) &&
            ((snapshot->obc.raw_data[0] & 0x03U) == 0x03U));
}

static bool zs_b1_has_charge_stop_condition(const app_bms_snapshot_t *snapshot,
                                            uint8_t detc)
{
    const t_charge_fault_A *charge_a =
        &snapshot->fault.active.detail.ucharge_fault_A.tcharge_fault_A;
    bool charge_context = ((detc != 0U) || (snapshot->current_10ma > 0));

    if (!charge_context) return false;
    return ((charge_a->b1full_charge_flag != 0U) ||
            zs_b1_has_charge_fault(snapshot) ||
            zs_b1_has_obc_stop_fault(snapshot, detc));
}

static void zs_b1_set_charge_mos_force(uint8_t enable)
{
    uint8_t requested = (enable != 0U) ? 1U : 0U;

    if (s_zs_b1_can.charge_mos_forced_off != requested)
    {
        app_h_power_manage_set_zs_charge_stop_override(requested);
        s_zs_b1_can.charge_mos_forced_off = requested;
    }
}

static void zs_b1_update_charger_stop(const app_bms_snapshot_t *snapshot,
                                      uint8_t detc)
{
    bool stop_condition = zs_b1_has_charge_stop_condition(snapshot, detc);

    if (detc == 0U)
    {
        s_zs_b1_can.charger_fault_report_latched = 0U;
        /* DETC不在位期间持续同步序号，重插时不把旧缓存帧当作新报文。 */
        s_zs_b1_can.last_obc_frame_sequence = snapshot->obc.frame_sequence;
        s_zs_b1_can.obc_frame_sequence_valid = 1U;
    }
    else if ((s_zs_b1_can.obc_frame_sequence_valid == 0U) ||
             (s_zs_b1_can.last_obc_frame_sequence != snapshot->obc.frame_sequence))
    {
        s_zs_b1_can.last_obc_frame_sequence = snapshot->obc.frame_sequence;
        s_zs_b1_can.obc_frame_sequence_valid = 1U;
        if (snapshot->obc.frame_valid != 0U)
        {
            if (snapshot->obc.status == APP_BMS_OBC_STATE_FAULT)
            {
                s_zs_b1_can.charger_fault_report_latched = 1U;
            }
            else if (snapshot->obc.status == APP_BMS_OBC_STATE_PRESENT)
            {
                /* 只有新收到的正常OBC帧，才清除此前由OBC故障导致的0x019状态。 */
                s_zs_b1_can.charger_fault_report_latched = 0U;
                if (s_zs_b1_can.charge_result_from_obc != 0U)
                {
                    s_zs_b1_can.charge_result_valid = 0U;
                    s_zs_b1_can.charge_result_status = 0U;
                    s_zs_b1_can.charge_result_from_obc = 0U;
                }
            }
        }
    }

    if ((s_zs_b1_can.charge_result_from_bms != 0U) &&
        !zs_b1_has_charge_fault(snapshot))
    {
        /* BMS充电保护恢复后，同步清除由该保护形成的0x019锁存结果。 */
        s_zs_b1_can.charge_result_valid = 0U;
        s_zs_b1_can.charge_result_status = 0U;
        s_zs_b1_can.charge_result_from_bms = 0U;
    }

    if (stop_condition && (s_zs_b1_can.charge_stop_commanded == 0U))
    {
        if (s_zs_b1_can.charge_stop_condition_ms < ZS_B1_CHARGER_STOP_DELAY_MS)
            s_zs_b1_can.charge_stop_condition_ms++;

        if (s_zs_b1_can.charge_stop_condition_ms >= ZS_B1_CHARGER_STOP_DELAY_MS)
        {
            s_zs_b1_can.charge_stop_commanded = 1U;
            s_zs_b1_can.charge_stop_waiting = 1U;
            s_zs_b1_can.charge_stop_wait_ms = 0U;
        }
    }
    else if ((stop_condition == false) &&
             (s_zs_b1_can.charge_stop_commanded == 0U))
    {
        s_zs_b1_can.charge_stop_condition_ms = 0U;
    }

    if (s_zs_b1_can.charge_stop_waiting != 0U)
    {
        if (snapshot->current_10ma <= 0)
        {
            /* 使用已有电流死区变量：<=0表示充电电流已回到零。 */
            s_zs_b1_can.charge_stop_waiting = 0U;
            s_zs_b1_can.charger_stop_confirmed = 1U;
            s_zs_b1_can.charge_result_valid = 1U;
            s_zs_b1_can.charge_result_from_obc =
                ((s_zs_b1_can.charger_fault_report_latched != 0U &&
                  !zs_b1_has_charge_fault(snapshot)) ? 1U : 0U);
            s_zs_b1_can.charge_result_from_bms =
                (zs_b1_has_charge_fault(snapshot) ? 1U : 0U);
            s_zs_b1_can.charge_result_status =
                ((zs_b1_has_charge_fault(snapshot) ||
                  (s_zs_b1_can.charger_fault_report_latched != 0U)) ? 1U :
                 ((snapshot->fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag != 0U)
                    ? 3U : 0U));
            zs_b1_set_charge_mos_force(1U);
        }
        else
        {
            if (s_zs_b1_can.charge_stop_wait_ms < ZS_B1_CHARGER_STOP_TIMEOUT_MS)
                s_zs_b1_can.charge_stop_wait_ms++;
            if (s_zs_b1_can.charge_stop_wait_ms >= ZS_B1_CHARGER_STOP_TIMEOUT_MS)
            {
                /* 停充指令后5秒电流仍为正，按充电机未执行处理并强制断开充电MOS。 */
                s_zs_b1_can.charge_stop_waiting = 0U;
                s_zs_b1_can.charger_stop_failed = 1U;
                s_zs_b1_can.charge_result_valid = 1U;
                s_zs_b1_can.charge_result_status = 1U;
                s_zs_b1_can.charge_result_from_obc = 0U;
                s_zs_b1_can.charge_result_from_bms = 0U;
                zs_b1_set_charge_mos_force(1U);
            }
        }
    }

    /* DETC移除且没有充电电流后，清除本轮停充锁存，允许下一轮重新充电。 */
    if ((detc == 0U) && (snapshot->current_10ma <= 0))
    {
        s_zs_b1_can.charge_stop_commanded = 0U;
        s_zs_b1_can.charge_stop_waiting = 0U;
        s_zs_b1_can.charger_stop_failed = 0U;
        s_zs_b1_can.charger_stop_confirmed = 0U;
        s_zs_b1_can.charge_result_valid = 0U;
        s_zs_b1_can.charge_result_status = 0U;
        s_zs_b1_can.charge_result_from_obc = 0U;
        s_zs_b1_can.charge_result_from_bms = 0U;
        s_zs_b1_can.charge_stop_condition_ms = 0U;
        s_zs_b1_can.charge_stop_wait_ms = 0U;
        zs_b1_set_charge_mos_force(0U);
    }

}

static void zs_b1_start_session(void)
{
    s_zs_b1_can.tx_active = 1U;
    s_zs_b1_can.idle_timeout_complete = 0U;
    s_zs_b1_can.session_idle_latched = 0U;
    s_zs_b1_can.low_current_ms = 0U;
    s_zs_b1_can.timer_status1_ms = 0U;
    s_zs_b1_can.timer_100ms_ms = 0U;
    s_zs_b1_can.timer_500ms_ms = 0U;
    s_zs_b1_can.due_100ms_mask = 0U;
    s_zs_b1_can.due_500ms_mask = 0U;
    s_zs_b1_can.next_cell_index = 0U;
}

static uint8_t zs_b1_fault_temp_level(uint16_t warning,
                                      uint16_t protection,
                                      uint16_t permanent)
{
    if (permanent != 0U) return 3U;
    if (protection != 0U) return 2U;
    if (warning != 0U) return 1U;
    return 0U;
}

static uint8_t zs_b1_fault_cell_uv_level(const t_bms_fault_flag *fault)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    if (fault_a->b1cell_deep_cuv != 0U) return 3U;
    if (fault_a->b1cell_pro_cuv2 != 0U) return 2U;
    if ((fault_a->b1cell_pro_cuv1 != 0U) || (fault_a->b1cell_war_cuv != 0U)) return 1U;
    return 0U;
}

static uint8_t zs_b1_fault_discharge_oc_level(const t_bms_fault_flag *fault)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *fault_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    if ((fault_a->b1pack_ocd_lock != 0U) ||
        (fault_b->b1pack_pro_ocd3 != 0U) || (fault_b->b1pack_pro_ocd4 != 0U)) return 3U;
    if (fault_b->b1pack_pro_ocd2 != 0U) return 2U;
    if (fault_b->b1pack_pro_ocd1 != 0U) return 1U;
    return 0U;
}

static uint8_t zs_b1_fault_charge_oc_level(const t_bms_fault_flag *fault)
{
    const t_charge_fault_A *fault_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *fault_b = &fault->ucharge_fault_B.tcharge_fault_B;
    if (fault_a->b1pack_pro_occ2 != 0U) return 2U;
    if ((fault_a->b1pack_pro_occ1 != 0U) || (fault_a->b1pack_pro_orcc != 0U) ||
        (fault_b->b1pack_occ_lock != 0U)) return 1U;
    return 0U;
}

static uint8_t zs_b1_fault_discharge_level(const t_bms_fault_flag *fault,
                                           uint8_t discharge_pro)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *fault_b = &fault->udischarge_fault_B.tdischarge_fault_B;

    /* 字段6仅区分放电告警和放电保护：00正常、10告警、11保护。 */
    if (discharge_pro != 0U) return 3U;
    if ((fault_a->b1cell_war_cuv != 0U) ||
        (fault_a->b1pack_war_otd != 0U) ||
        (fault_a->b1pack_war_utd != 0U) ||
        (fault_a->b1pack_war_ocd != 0U) ||
        (fault_a->b1low_soc != 0U) ||
        (fault_b->b1pack_war_cuv != 0U))
    {
        return 2U;
    }
    return 0U;
}

static bool zs_b1_fault_any(const app_bms_fault_image_t *fault)
{
    return ((fault->charge_pro != 0U) || (fault->discharge_pro != 0U) ||
            (fault->general_fault != 0U) || (fault->pf_fault != 0U));
}

static bool zs_b1_send_fault(const app_bms_snapshot_t *snapshot)
{
    const t_bms_fault_flag *fault = &snapshot->fault.active.detail;
    const t_charge_fault_A *charge_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &fault->ucharge_fault_B.tcharge_fault_B;
    const t_discharge_fault_A *discharge_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *discharge_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    const t_general_fault_A *general_a = &fault->ugeneral_fault_A.tgeneral_fault_A;
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint8_t discharge_oc_level = zs_b1_fault_discharge_oc_level(fault);
    uint8_t charge_oc_level = zs_b1_fault_charge_oc_level(fault);
    uint8_t discharge_temp_level = zs_b1_fault_temp_level(discharge_a->b1pack_war_otd,
                                                           discharge_a->b1pack_pro_otd,
                                                           general_a->b1OTD_FUSE);
    uint8_t discharge_uv_level = zs_b1_fault_cell_uv_level(fault);
    uint8_t discharge_under_temp_level = zs_b1_fault_temp_level(discharge_a->b1pack_war_utd,
                                                                 discharge_a->b1pack_pro_utd,
                                                                 0U);
    uint8_t charge_under_temp_level = zs_b1_fault_temp_level(charge_a->b1pack_war_utc,
                                                              charge_a->b1pack_pro_utc,
                                                              0U);
    uint8_t charge_temp_level = zs_b1_fault_temp_level(charge_a->b1pack_war_otc,
                                                        charge_a->b1pack_pro_otc,
                                                        0U);
    uint8_t discharge_fault_level = zs_b1_fault_discharge_level(
        fault, snapshot->fault.active.discharge_pro);
    uint8_t work_mode = 0U;
    uint8_t charge_status = 0U;
    uint8_t power_status = 0U;

    /* 此客户矩阵标注校验暂不实现，故校验字节按0填充。 */
    data[0] = 0U;
    data[1] = (uint8_t)(s_zs_b1_can.counter_fault & 0x0FU);
    if (zs_b1_fault_any(&snapshot->fault.active))
    {
        /* 任一当前保护/通用故障/PF故障优先于普通工作状态。 */
        work_mode = 3U;
    }
    else
    {
        switch (snapshot->bms_work_status)
        {
            case APP_BMS_STATUS_DISCHARGE:
            case APP_BMS_STATUS_FEEDBACK:
                work_mode = 1U;
                break;
            case APP_BMS_STATUS_CHARGE:
                work_mode = 2U;
                break;
            case APP_BMS_STATUS_STATIC:
            default:
                work_mode = 0U;
                break;
        }
    }
    data[1] |= (uint8_t)(work_mode << 4U);
    /* Data1 bit6: DETC在位为1，不在位为0。 */
    if (snapshot->signal.input.mcu_detc != 0U) data[1] |= 0x40U;

    /* 停充流程结束后锁存最终结果；超时故障优先于满充完成状态。 */
    if ((s_zs_b1_can.charge_result_valid != 0U) &&
        (s_zs_b1_can.charge_result_status == 1U))
    {
        charge_status = s_zs_b1_can.charge_result_status;
    }
    else if ((s_zs_b1_can.charger_stop_failed != 0U) ||
             (s_zs_b1_can.charger_fault_report_latched != 0U))
    {
        charge_status = 1U;
    }
    else if (s_zs_b1_can.charge_result_valid != 0U)
    {
        charge_status = s_zs_b1_can.charge_result_status;
    }
    else if (charge_a->b1full_charge_flag != 0U)
    {
        charge_status = 3U;
    }
    else if ((snapshot->fault.active.charge_pro != 0U) ||
             (s_zs_b1_can.charger_fault_report_latched != 0U))
    {
        charge_status = 1U;
    }
    else if (snapshot->bms_work_status == APP_BMS_STATUS_CHARGE)
    {
        charge_status = 2U;
    }

    /* 状态反馈只表示DMOS；AFE状态无效时按协议发00。 */
    if (snapshot->afe_status_valid == 0U)
    {
        power_status = 0U;
    }
    else if (snapshot->mos.dsg_mos_state != 0U)
    {
        power_status = 1U;
    }
    else
    {
        power_status = 2U;
    }
    data[2] = (uint8_t)((discharge_fault_level & 0x03U) |
                        ((charge_status & 0x03U) << 2U) |
                        ((power_status & 0x03U) << 4U));

    /* Data3 bit0..3：总压过压/欠压状态，来源分别是总压保护标志。 */
    if (charge_b->b1pack_pro_pov != 0U) data[3] |= 0x01U;
    if (discharge_b->b1pack_pro_cuv != 0U) data[3] |= 0x02U;
    if (charge_b->b1pack_pro_pov != 0U) data[3] |= 0x04U;
    if (discharge_b->b1pack_pro_cuv != 0U) data[3] |= 0x08U;
    /* Data3 bit4/5：充/放电单体过压；两种工作状态共用单体过压保护源。 */
    if ((charge_a->b1cell_pro_cov != 0U) || (charge_a->b1cell_pro_cov2 != 0U))
        data[3] |= 0x30U;
    /* Data3 bit6/7：充电单体欠压等级；Data4 bit0/1：放电单体欠压等级。 */
    data[3] |= (uint8_t)((discharge_uv_level & 0x03U) << 6U);
    data[4] = (uint8_t)((discharge_uv_level & 0x03U) |
                        ((discharge_temp_level & 0x03U) << 2U) |
                        ((charge_temp_level & 0x03U) << 4U) |
                        ((discharge_under_temp_level & 0x03U) << 6U));
    data[5] = (uint8_t)((charge_under_temp_level & 0x03U) |
                        ((charge_oc_level & 0x03U) << 2U) |
                        ((discharge_oc_level & 0x03U) << 4U));
    /* 字段23：二级保护优先；一级保护或仅告警时上报一级。 */
    if (general_a->b1cell_voltage_diff_fault2 != 0U) data[5] |= 0x80U;
    else if ((general_a->b1cell_voltage_diff_fault != 0U) ||
             (general_a->b1cell_voltage_diff_waring != 0U)) data[5] |= 0x40U;

    /* Data6 bit0..4（字段24..26及保留位）不映射，保持0。 */
    if (general_a->b1MOS_OTP != 0U) data[6] |= 0x20U;
    if (snapshot->fault.active.pf_fault != 0U) data[6] |= 0x40U;
    if (discharge_b->b1pack_shrot != 0U) data[6] |= 0x80U;

    /* Data7字段30、32..34暂不实现；字段31仅报告MOS损坏PF。 */
    if ((general_a->b1CHG_MOS_failure != 0U) || (general_a->b1DSG_MOS_faulure != 0U))
        data[7] |= 0x02U;

    if (!zs_b1_send(ZS_B1_ID_BMS_FAULT, data)) return false;
    s_zs_b1_can.counter_fault = (uint8_t)((s_zs_b1_can.counter_fault + 1U) & 0x0FU);
    return true;
}

static uint16_t zs_b1_sop_01a_to_025a(int16_t current_01a)
{
    uint32_t current_ma;
    if (current_01a <= 0) return 0U;
    current_ma = (uint32_t)(uint16_t)current_01a * 100U;
    return zs_b1_limit_u12(current_ma / 250U);
}

static bool zs_b1_send_status1(const app_bms_snapshot_t *snapshot)
{
    const t_general_fault_A *general = &snapshot->fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    const t_charge_fault_A *charge_a = &snapshot->fault.active.detail.ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &snapshot->fault.active.detail.ucharge_fault_B.tcharge_fault_B;
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint32_t discharge_ma = (snapshot->current_business_ma < 0) ? zs_b1_abs_current_ma(snapshot->current_business_ma) : 0U;
    uint16_t current_025a = zs_b1_limit_u12(discharge_ma / 250U);
    uint16_t max_discharge_025a = zs_b1_sop_01a_to_025a(snapshot->sop.s16MaxDischargeCurrent10mA);
    uint16_t max_feedback_025a;
    uint8_t status = 0U;
    bool data_valid = ((snapshot->data_ready != 0U) &&
                       (snapshot->cell_count != 0U) &&
                       (snapshot->valid_cells >= snapshot->cell_count));
    bool no_feedback_protection =
        ((charge_a->b1pack_pro_orcc != 0U) ||
         (charge_a->b1cell_pro_cov != 0U) ||
         (charge_a->b1cell_pro_cov2 != 0U) ||
         (charge_b->b1pack_pro_pov != 0U) ||
         (snapshot->afe_fault.cell_ov != 0U));
    max_feedback_025a = no_feedback_protection ? 0U :
        zs_b1_limit_u12(((uint32_t)snapshot->sop.u16MaxRechargeCurrent10mA * 100U) / 250U);

    if (!data_valid) data[0] |= 0x01U;
    if ((general->b1AFE_fault != 0U) || (general->b1hardware_second_pro != 0U)) data[0] |= 0x02U;
    if (general->b1DSG_MOS_faulure != 0U) data[0] |= 0x04U;
    if (general->b1CHG_MOS_failure != 0U) data[0] |= 0x08U;

    if (!data_valid || zs_b1_fault_any(&snapshot->fault.active)) status = 2U;
    else if (snapshot->bms_work_status == APP_BMS_STATUS_CHARGE) status = 1U;
    else if (snapshot->bms_work_status == APP_BMS_STATUS_DISCHARGE) status = 4U;
    else if (snapshot->bms_work_status == APP_BMS_STATUS_FEEDBACK) status = 8U;
    data[0] |= (uint8_t)(status << 4U);

    /* 放电电流：bit8..19，0.25A/bit。 */
    data[1] = (uint8_t)(current_025a & 0xFFU);
    data[2] = (uint8_t)((current_025a >> 8U) & 0x0FU);
    /* 最大放电能力：bit20..31，0.25A/bit。 */
    data[2] |= (uint8_t)((max_discharge_025a & 0x0FU) << 4U);
    data[3] = (uint8_t)(max_discharge_025a >> 4U);
    /* 最大回馈能力：bit32..43，0.25A/bit。 */
    data[4] = (uint8_t)(max_feedback_025a & 0xFFU);
    data[5] = (uint8_t)((max_feedback_025a >> 8U) & 0x0FU);
    if (!data_valid) data[5] |= 0x10U | 0x20U;
    if (no_feedback_protection) data[5] |= 0x40U;
    data[6] = (uint8_t)((s_zs_b1_can.counter_status1 & 0x0FU) << 4U);

    /* 矩阵未给出此帧校验算法，按未实现处理，校验字节置0。 */
    data[7] = 0U;
    if (!zs_b1_send(ZS_B1_ID_BMS_STATUS1, data)) return false;
    s_zs_b1_can.counter_status1 = (uint8_t)((s_zs_b1_can.counter_status1 + 1U) & 0x0FU);
    return true;
}

static bool zs_b1_send_status2(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint32_t fcc_ah = (snapshot->soc.s32battery_full_cap > 0)
        ? (uint32_t)snapshot->soc.s32battery_full_cap / 1000U
        : (uint32_t)APP_PARAM_BAT_CAPACITY / 1000U;
    uint32_t rated_voltage_12v = (uint32_t)APP_PARAM_BAT_VOLTAGE / 12000U;
    uint16_t remaining_minutes = app_charge_time_get_minutes();
    if (remaining_minutes == APP_CHARGE_TIME_INVALID_MINUTES)
        remaining_minutes = 0x0FFFU; /* 协议约定：无有效估算值为12-bit全1。 */
    else if (remaining_minutes >= 0x0FFFU)
        remaining_minutes = 0x0FFEU; /* 最大有效值须避开无效码。 */

    zs_b1_put_u16_le(data, 0U, zs_b1_limit_u16(snapshot->soc.u32cycle_index));
    data[2] = (uint8_t)(remaining_minutes & 0xFFU);
    data[3] = (uint8_t)(((remaining_minutes >> 8U) & 0x0FU) |
                        ((rated_voltage_12v & 0x0FU) << 4U));
    data[4] = zs_b1_limit_u8((uint32_t)APP_PARAM_BAT_CAPACITY / 1000U);
    data[5] = zs_b1_limit_u8(fcc_ah);
    data[6] = zs_b1_limit_u8((uint32_t)((snapshot->soc.u8display_soc > 100U)
                                           ? 200U : snapshot->soc.u8display_soc * 2U));
    data[7] = snapshot->soc.u8display_soh;
    return zs_b1_send(ZS_B1_ID_BMS_STATUS2, data);
}

static bool zs_b1_send_charge_control(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint16_t max_charge_005a = zs_b1_limit_charge_current_005a(
        (snapshot->sop.s16MaxChargeCurrent10mA > 0)
            ? (uint32_t)(uint16_t)snapshot->sop.s16MaxChargeCurrent10mA * 2U : 0U);
    uint32_t actual_charge_ma = (snapshot->current_business_ma > 0) ? (uint32_t)snapshot->current_business_ma : 0U;
    uint16_t actual_charge_005a = zs_b1_limit_charge_current_005a(actual_charge_ma / 50U);
    uint16_t min_charge_voltage_001v = 0U;
    uint16_t max_charge_voltage_001v = zs_b1_limit_u16(snapshot->sop.u32MaxChargeVoltageMv / 10U);

    if ((snapshot->data_ready != 0U) && (snapshot->cell_count != 0U) &&
        (snapshot->valid_cells >= snapshot->cell_count) && (snapshot->bat_mv != 0U))
    {
        min_charge_voltage_001v = zs_b1_limit_u16((snapshot->bat_mv / 10U) + 200U);
    }

    /* MaxChargeCurrent bit0..11; CurrentChargeCurrent bit12..23. */
    data[0] = (uint8_t)(max_charge_005a & 0xFFU);
    data[1] = (uint8_t)(((max_charge_005a >> 8U) & 0x0FU) |
                        ((actual_charge_005a & 0x0FU) << 4U));
    data[2] = (uint8_t)(actual_charge_005a >> 4U);
    /* Minimum charge voltage: valid real-time pack voltage + 2V; invalid snapshot sends 0. */
    zs_b1_put_u16_le(data, 3U, min_charge_voltage_001v);
    zs_b1_put_u16_le(data, 5U, max_charge_voltage_001v);
    /* BMS_MotorControl置位表示已按充电结束/故障流程请求充电机停机。 */
    if (s_zs_b1_can.charge_stop_commanded != 0U) data[7] |= 0x01U;
    if (snapshot->mos.chg_mos_state != 0U) data[7] |= 0x02U;
    if (snapshot->mos.dsg_mos_state != 0U) data[7] |= 0x04U;
    data[7] |= (uint8_t)((s_zs_b1_can.counter_charge_control & 0x0FU) << 4U);

    if (!zs_b1_send(ZS_B1_ID_BMS_CHARGE_CONTROL, data)) return false;
    s_zs_b1_can.counter_charge_control = (uint8_t)((s_zs_b1_can.counter_charge_control + 1U) & 0x0FU);
    return true;
}

static uint8_t zs_b1_cell_number(uint8_t zero_based_index)
{
    return (zero_based_index < 254U) ? (uint8_t)(zero_based_index + 1U) : 0xFFU;
}

static bool zs_b1_send_status3(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint16_t pack_voltage_001v = 0xFFFFU;
    uint16_t min_cell_mv = 0xFFFFU;
    uint16_t max_cell_mv = 0xFFFFU;

    if ((snapshot->data_ready != 0U) && (snapshot->valid_cells >= snapshot->cell_count))
    {
        pack_voltage_001v = zs_b1_limit_u16(snapshot->bat_mv / 10U);
        min_cell_mv = snapshot->min_cell_mv;
        max_cell_mv = snapshot->max_cell_mv;
    }
    zs_b1_put_u16_le(data, 0U, pack_voltage_001v);
    zs_b1_put_u16_le(data, 2U, min_cell_mv);
    data[4] = (min_cell_mv == 0xFFFFU) ? 0xFFU : zs_b1_cell_number(snapshot->min_cell_num);
    zs_b1_put_u16_le(data, 5U, max_cell_mv);
    data[7] = (max_cell_mv == 0xFFFFU) ? 0xFFU : zs_b1_cell_number(snapshot->max_cell_num);
    return zs_b1_send(ZS_B1_ID_BMS_STATUS3, data);
}

static uint8_t zs_b1_temp_value(int16_t temperature_c)
{
    int32_t value;
    if (temperature_c == APP_BMS_TEMP_INVALID_VALUE) return 0xFFU;
    value = (int32_t)temperature_c + 40;
    if (value < 0) return 0U;
    /* 0xFF保留为无效值，故有效温度最大编码为0xFE（214°C）。 */
    if (value > 254) return 254U;
    return (uint8_t)value;
}

static int16_t zs_b1_tenth_c_to_floor_c(int16_t temperature_0p1c)
{
    if (temperature_0p1c >= 0) return (int16_t)(temperature_0p1c / 10);
    return (int16_t)(-((-(int32_t)temperature_0p1c + 9) / 10));
}

static uint8_t zs_b1_temperature_index_nibble(uint8_t zero_based_index, bool valid)
{
    if ((!valid) || (zero_based_index >= 14U)) return 0x0FU;
    return (uint8_t)(zero_based_index + 1U);
}

static bool zs_b1_send_status4(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint8_t index;
    uint8_t valid_count = 0U;
    int32_t temperature_sum = 0;
    bool mos_valid = ((snapshot->ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U);
    int16_t mos_temperature_c = mos_valid
        ? zs_b1_tenth_c_to_floor_c((int16_t)snapshot->ntc.mos_temp_0p1c)
        : APP_BMS_TEMP_INVALID_VALUE;

    for (index = 0U; index < APP_BMS_MAX_TEMP_NUM; index++)
    {
        if ((snapshot->temp_valid_mask & (uint8_t)(1U << index)) != 0U)
        {
            temperature_sum += snapshot->temp_c[index];
            valid_count++;
        }
    }
    data[0] = (valid_count == 0U) ? 0xFFU : zs_b1_temp_value((int16_t)(temperature_sum / valid_count));
    data[1] = (uint8_t)(zs_b1_temperature_index_nibble(snapshot->min_temp_num, valid_count != 0U) |
                        (zs_b1_temperature_index_nibble(snapshot->max_temp_num, valid_count != 0U) << 4U));
    data[2] = zs_b1_temp_value(snapshot->min_temp_c);
    data[3] = zs_b1_temp_value(snapshot->max_temp_c);
    data[4] = zs_b1_temp_value(mos_temperature_c);
    data[5] = zs_b1_temp_value(mos_temperature_c);
    data[6] = (uint8_t)(zs_b1_temperature_index_nibble(0U, mos_valid) |
                        (zs_b1_temperature_index_nibble(0U, mos_valid) << 4U));
    /* byte7 reserved */
    return zs_b1_send(ZS_B1_ID_BMS_STATUS4, data);
}

static void zs_b1_parse_version_hl(const char *version, uint8_t *high, uint8_t *low)
{
    uint8_t digits[2] = {0U, 0U};
    uint8_t digit_count = 0U;

    *high = 0U;
    *low = 0U;
    if (version == 0) return;
    while (*version != '\0')
    {
        if ((*version >= '0') && (*version <= '9'))
        {
            if (digit_count < 2U)
            {
                digits[digit_count] = (uint8_t)(*version - '0');
                digit_count++;
            }
        }
        version++;
    }

    if (digit_count == 1U)
    {
        *low = digits[0];
    }
    else if (digit_count >= 2U)
    {
        *high = digits[0];
        *low = digits[1];
    }
}

static uint8_t zs_b1_parse_version_value(const char *version)
{
    uint16_t value = 0U;

    if (version == 0) return 0U;
    while (*version != '\0')
    {
        if ((*version >= '0') && (*version <= '9'))
        {
            value = (uint16_t)((value * 10U) + (uint16_t)(*version - '0'));
            /* Protocol version components are limited to the documented 0..15 range. */
            if (value > 15U) return 15U;
        }
        version++;
    }
    return (uint8_t)value;
}

static bool zs_b1_send_status5(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint8_t index = s_zs_b1_can.next_cell_index;
    uint16_t cell_mv = 0xFFFFU;
    uint8_t hw_version_high;
    uint8_t hw_version_low;
    uint8_t sw_version_high;
    uint8_t sw_version_low;

    zs_b1_parse_version_hl(APP_PARAM_HW_VERSION, &hw_version_high, &hw_version_low);
    sw_version_high = zs_b1_parse_version_value(APP_PARAM_PACKAGE_V_VERSION);
    sw_version_low = zs_b1_parse_version_value(APP_PARAM_PACKAGE_T_VERSION);

    if ((snapshot->data_ready != 0U) && (snapshot->valid_cells >= snapshot->cell_count) &&
        (snapshot->cell_count != 0U) && (index < snapshot->cell_count))
    {
        data[0] = zs_b1_cell_number(index);
        cell_mv = snapshot->cell_mv[index];
    }
    else
    {
        data[0] = 0xFFU;
    }
    zs_b1_put_u16_le(data, 1U, cell_mv);
    /* byte3 reserved; bytes4..7 are H/L hardware and software version values. */
    data[4] = hw_version_high;
    data[5] = hw_version_low;
    data[6] = sw_version_high;
    data[7] = sw_version_low;

    if (!zs_b1_send(ZS_B1_ID_BMS_STATUS5, data)) return false;
    if ((snapshot->data_ready != 0U) && (snapshot->valid_cells >= snapshot->cell_count) &&
        (snapshot->cell_count != 0U))
    {
        s_zs_b1_can.next_cell_index++;
        if (s_zs_b1_can.next_cell_index >= snapshot->cell_count) s_zs_b1_can.next_cell_index = 0U;
    }
    return true;
}

static void zs_b1_tick_timer(uint16_t *timer, uint16_t period)
{
    if (*timer < period) (*timer)++;
}

static void zs_b1_schedule_periodic(const app_bms_snapshot_t *snapshot)
{
    zs_b1_tick_timer(&s_zs_b1_can.timer_status1_ms, ZS_B1_PERIOD_STATUS1_MS);
    if (s_zs_b1_can.timer_status1_ms >= ZS_B1_PERIOD_STATUS1_MS)
    {
        if (zs_b1_send_status1(snapshot)) s_zs_b1_can.timer_status1_ms = 0U;
    }

    if (s_zs_b1_can.timer_100ms_ms < ZS_B1_PERIOD_100MS)
    {
        s_zs_b1_can.timer_100ms_ms++;
        if (s_zs_b1_can.timer_100ms_ms == ZS_B1_PERIOD_100MS)
            s_zs_b1_can.due_100ms_mask = 0x03U;
    }
    if (s_zs_b1_can.timer_100ms_ms >= ZS_B1_PERIOD_100MS)
    {
        if (((s_zs_b1_can.due_100ms_mask & 0x01U) != 0U) && zs_b1_send_fault(snapshot))
            s_zs_b1_can.due_100ms_mask &= (uint8_t)~0x01U;
        if (((s_zs_b1_can.due_100ms_mask & 0x02U) != 0U) && zs_b1_send_charge_control(snapshot))
            s_zs_b1_can.due_100ms_mask &= (uint8_t)~0x02U;
        if (s_zs_b1_can.due_100ms_mask == 0U) s_zs_b1_can.timer_100ms_ms = 0U;
    }

    if (s_zs_b1_can.timer_500ms_ms < ZS_B1_PERIOD_500MS)
    {
        s_zs_b1_can.timer_500ms_ms++;
        if (s_zs_b1_can.timer_500ms_ms == ZS_B1_PERIOD_500MS)
            s_zs_b1_can.due_500ms_mask = 0x0FU;
    }
    if (s_zs_b1_can.timer_500ms_ms >= ZS_B1_PERIOD_500MS)
    {
        if (((s_zs_b1_can.due_500ms_mask & 0x01U) != 0U) && zs_b1_send_status2(snapshot))
            s_zs_b1_can.due_500ms_mask &= (uint8_t)~0x01U;
        if (((s_zs_b1_can.due_500ms_mask & 0x02U) != 0U) && zs_b1_send_status3(snapshot))
            s_zs_b1_can.due_500ms_mask &= (uint8_t)~0x02U;
        if (((s_zs_b1_can.due_500ms_mask & 0x04U) != 0U) && zs_b1_send_status4(snapshot))
            s_zs_b1_can.due_500ms_mask &= (uint8_t)~0x04U;
        if (((s_zs_b1_can.due_500ms_mask & 0x08U) != 0U) && zs_b1_send_status5(snapshot))
            s_zs_b1_can.due_500ms_mask &= (uint8_t)~0x08U;
        if (s_zs_b1_can.due_500ms_mask == 0U) s_zs_b1_can.timer_500ms_ms = 0U;
    }
}

void zs_b1_can_protocol_init(void)
{
    memset(&s_zs_b1_can, 0, sizeof(s_zs_b1_can));
}

void zs_b1_can_protocol_task(void)
{
    app_bms_snapshot_t snapshot;
    uint8_t detd;
    uint8_t detc;
    uint8_t key_present;
    bool input_rising;
    uint8_t has_business_current;
    bool charge_context;
    bool charge_event;

    app_bms_snapshot_read(&snapshot);
    detd = (snapshot.signal.input.mcu_detd != 0U) ? 1U : 0U;
    detc = (snapshot.signal.input.mcu_detc != 0U) ? 1U : 0U;
    key_present = (snapshot.signal.input.mcu_key_present != 0U) ? 1U : 0U;
    input_rising = (((detd != 0U) && (s_zs_b1_can.previous_detd == 0U)) ||
                    ((detc != 0U) && (s_zs_b1_can.previous_detc == 0U)));
    s_zs_b1_can.previous_detd = detd;
    s_zs_b1_can.previous_detc = detc;
    has_business_current = (snapshot.current_10ma != 0) ? 1U : 0U;
    charge_context = ((detc != 0U) || (snapshot.current_10ma > 0));
    zs_b1_update_charger_stop(&snapshot, detc);
    charge_event = (charge_context &&
                    ((snapshot.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag != 0U) ||
                     zs_b1_has_charge_fault(&snapshot) ||
                     (s_zs_b1_can.charger_fault_report_latched != 0U)));

    /* KEY在位允许会话立即启动；电流或插拔沿仍按原逻辑解除空闲锁存。 */
    if (key_present != 0U)
    {
        s_zs_b1_can.session_idle_latched = 0U;
    }
    if ((input_rising != false) || (has_business_current != 0U))
    {
        s_zs_b1_can.session_idle_latched = 0U;
        s_zs_b1_can.idle_timeout_complete = 0U;
    }

    if ((s_zs_b1_can.tx_active == 0U) &&
        (s_zs_b1_can.session_idle_latched == 0U) &&
        ((key_present != 0U) || input_rising ||
         (has_business_current != 0U) || charge_event))
    {
        zs_b1_start_session();
    }

    if (s_zs_b1_can.tx_active == 0U) return;

    if ((input_rising != false) || (has_business_current != 0U))
    {
        s_zs_b1_can.low_current_ms = 0U;
    }
    else if (s_zs_b1_can.low_current_ms < APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS)
    {
        s_zs_b1_can.low_current_ms++;
    }

    if (s_zs_b1_can.low_current_ms >= APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS)
    {
        s_zs_b1_can.idle_timeout_complete = 1U;
        /* KEY持续在位时忽略电流空闲停止；KEY缺失时仍按原60秒规则停发。 */
        if (key_present == 0U)
        {
            s_zs_b1_can.tx_active = 0U;
            s_zs_b1_can.session_idle_latched = 1U;
            s_zs_b1_can.due_100ms_mask = 0U;
            s_zs_b1_can.due_500ms_mask = 0U;
            return;
        }
    }

    zs_b1_schedule_periodic(&snapshot);
}

bool zs_b1_can_protocol_allows_sleep(void)
{
    return (s_zs_b1_can.tx_active == 0U);
}

bool zs_b1_can_protocol_idle_timeout_complete(void)
{
    return ((s_zs_b1_can.idle_timeout_complete != 0U) &&
            (s_zs_b1_can.tx_active == 0U));
}

void zs_b1_can_protocol_notify_wakeup(void)
{
    zs_b1_start_session();
}
