/*
 * 宗申多电池 CAN B1 协议：只负责客户侧标准帧主动发送，不接管 DUCAR 扩展帧。
 * 由 Comm_Task 周期调用；所有周期和超时均使用TIM6实际毫秒差值累计。
 * 主动通信期间0x419按500ms周期发送Active=0；IGN/充电使能变化后先发11帧Wakeup=2；
 * KEY缺失且业务电流为零时，等待窗口开始后0x419周期发送Sleep=1；连续20秒后停止其余周期报文并结束主动通信。
 * 快照每10ms更新，协议始终读取一致副本。
 */
#include "Protocol/Comm/zs_b1_can_protocol.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "App/app_bms_snapshot.h"
#include "App/app_bms_fault_config.h"
#include "App/app_charge_time.h"
#include "App/app_h_power_manage.h"
#include "App/app_bms_status.h"
#include "App/app_param.h"
#include "Driver/d_can.h"
#include "Driver/d_timer.h"

#define ZS_B1_CAN_DLC                 8U
/* 启用宗申B1标准帧定时主动发送。 */
#define ZS_B1_ACTIVE_TX_ENABLE        1U
/* 发送ID：网络管理、故障、状态1~5和充电控制；均为8字节CAN标准帧。 */
#define ZS_B1_ID_BMS_NM_REQUEST       0x419U
#define ZS_B1_ID_BMS_FAULT            0x019U
#define ZS_B1_ID_BMS_STATUS1          0x089U
#define ZS_B1_ID_BMS_STATUS2          0x619U
#define ZS_B1_ID_BMS_CHARGE_CONTROL   0x621U
#define ZS_B1_ID_BMS_STATUS3          0x519U
#define ZS_B1_ID_BMS_STATUS4          0x5A0U
#define ZS_B1_ID_BMS_STATUS5          0x5ABU

/* 0x089为20ms；0x019/0x621为100ms；其余状态帧为500ms。 */
#define ZS_B1_PERIOD_STATUS1_MS       20U
#define ZS_B1_PERIOD_100MS             100U
#define ZS_B1_PERIOD_500MS             500U
#define ZS_B1_NM_WAKEUP_FRAME_COUNT   11U
#define ZS_B1_NM_STATE_SLEEP          0x01U
#define ZS_B1_NM_STATE_ACTIVE         0x00U
#define ZS_B1_NM_STATE_WAKEUP         0x02U
#define ZS_B1_NM_STATE_NONE           0xFFU
#define ZS_B1_CHARGER_STOP_DELAY_MS    10000UL
#define ZS_B1_CHARGER_STOP_TIMEOUT_MS  5000UL

/* 本模块运行状态；所有时间计数根据TIM6的实际毫秒差值更新。 */
typedef struct
{
    uint8_t tx_active;                    /* 主动发送会话是否运行。 */
    uint8_t idle_timeout_complete;        /* 电流空闲计时是否到期。 */
    uint8_t due_nm_state;                 /* 0x419待重发状态；NONE表示无待发帧。 */
    uint8_t wakeup_frames_remaining;      /* 0x419 Wakeup帧待成功发送数量。 */
    uint8_t nm_active_after_wakeup;       /* 11帧Wakeup后必须先发一帧Active。 */
    uint8_t nm_sleep_requested;           /* KEY和电流均空闲时，周期0x419发送Sleep。 */
    uint8_t nm_sleep_sent;                /* 当前空闲窗口内Sleep是否已被CAN发送接口接受。 */
    uint8_t charge_stop_commanded;        /* 已向充电机请求停充。 */
    uint8_t charge_stop_waiting;          /* 等待充电电流降至零。 */
    uint8_t charger_stop_failed;          /* 停充后等待电流归零超时。 */
    uint8_t charger_stop_confirmed;        /* 已通过电流归零确认停充。 */
    uint8_t charger_fault_report_latched; /* 锁存充电机故障上报状态。 */
    uint8_t charge_result_valid;          /* 停充结果是否有效。 */
    uint8_t charge_result_status;         /* 0x019中的停充结果状态编码。 */
    uint8_t charge_result_from_obc;       /* 结果是否由充电机故障产生。 */
    uint8_t charge_result_from_bms;       /* 结果是否由BMS充电故障产生。 */
    uint8_t charge_mos_forced_off;        /* 是否已请求强制关闭充电MOS。 */
    uint8_t session_idle_latched;         /* 空闲停发锁存，防止立即重复启动。 */
    uint8_t obc_frame_sequence_valid;     /* 是否已记录充电机报文序号。 */
    uint8_t previous_detd;                /* 上次放电检测输入，用于检测上升沿。 */
    uint8_t previous_detc;                /* 上次充电检测输入，用于检测上升沿。 */
    uint8_t counter_fault;                /* 0x019滚动计数。 */
    uint8_t counter_status1;              /* 0x089滚动计数。 */
    uint8_t counter_charge_control;       /* 0x621滚动计数。 */
    uint8_t next_cell_index;              /* 0x5AB下一次发送的逻辑电芯下标。 */
    uint8_t due_100ms_mask;               /* 本轮100ms报文的待发送位图。 */
    uint8_t due_500ms_mask;               /* 本轮500ms报文的待发送位图。 */
    uint8_t task_time_valid;              /* 是否已记录上一次任务的TIM6时间戳。 */
    uint32_t low_current_ms;              /* KEY缺失且无业务电流时的停发等待时间。 */
    uint32_t charge_stop_condition_ms;    /* 停充条件连续成立时间。 */
    uint32_t charge_stop_wait_ms;         /* 发出停充请求后等待电流归零时间。 */
    uint32_t last_obc_frame_sequence;     /* 上次处理的充电机报文序号。 */
    uint32_t last_task_ms;                /* 上一次任务调用对应的TIM6毫秒时间戳。 */
    uint16_t timer_status1_ms;            /* 20ms报文计时器。 */
    uint16_t timer_100ms_ms;              /* 100ms报文组计时器。 */
    uint16_t timer_500ms_ms;              /* 500ms报文组计时器。 */
    uint16_t timer_network_mgmt_ms;       /* 0x419固定500ms周期计时器。 */
} zs_b1_can_state_t;

static zs_b1_can_state_t s_zs_b1_can;

static void zs_b1_tick_timer(uint16_t *timer, uint16_t period, uint32_t elapsed_ms);

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

/* 累加实际经过的毫秒数，并将计时值限制在对应超时门限。 */
static void zs_b1_add_elapsed_ms(uint32_t *timer, uint32_t elapsed_ms, uint32_t limit_ms)
{
    if (*timer >= limit_ms) return;
    if (elapsed_ms >= (limit_ms - *timer))
        *timer = limit_ms;
    else
        *timer += elapsed_ms;
}

static void zs_b1_put_u16_le(uint8_t *data, uint8_t offset, uint16_t value)
{
    data[offset] = (uint8_t)(value & 0xFFU);
    data[offset + 1U] = (uint8_t)(value >> 8U);
}

/* 通用标准帧发送入口：发送ID由调用方的id参数指定。 */
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

/* 发送ID：0x419，500ms网络管理报文；Data0=1睡眠、0=Active、2=Wakeup。 */
static bool zs_b1_send_network_management(uint8_t state)
{
    uint8_t data[ZS_B1_CAN_DLC] =
        {state, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};

    return zs_b1_send(ZS_B1_ID_BMS_NM_REQUEST, data);
}

/* 到达0x419周期点时尝试发送；CAN忙则保留待发状态，后续任务重试。 */
static void zs_b1_flush_network_management(void)
{
    if (s_zs_b1_can.due_nm_state != ZS_B1_NM_STATE_NONE)
    {
        if (zs_b1_send_network_management(s_zs_b1_can.due_nm_state))
        {
            if ((s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_WAKEUP) &&
                (s_zs_b1_can.wakeup_frames_remaining > 0U))
            {
                s_zs_b1_can.wakeup_frames_remaining--;
            }
            if ((s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_ACTIVE) &&
                (s_zs_b1_can.nm_active_after_wakeup != 0U))
            {
                s_zs_b1_can.nm_active_after_wakeup = 0U;
            }
            if (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_SLEEP)
            {
                s_zs_b1_can.nm_sleep_sent = 1U;
            }
            s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_NONE;
            s_zs_b1_can.timer_network_mgmt_ms = 0U;
        }
    }
}

/* 0x419只在固定500ms周期点发送，输入事件仅改变该周期帧的状态字段。 */
static void zs_b1_schedule_network_management(uint32_t elapsed_ms)
{
    if (s_zs_b1_can.timer_network_mgmt_ms < ZS_B1_PERIOD_500MS)
    {
        zs_b1_tick_timer(&s_zs_b1_can.timer_network_mgmt_ms,
                         ZS_B1_PERIOD_500MS,
                         elapsed_ms);
    }

    if (s_zs_b1_can.timer_network_mgmt_ms >= ZS_B1_PERIOD_500MS)
    {
        if (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_NONE)
        {
            if (s_zs_b1_can.wakeup_frames_remaining > 0U)
            {
                s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_WAKEUP;
            }
            else if (s_zs_b1_can.nm_active_after_wakeup != 0U)
            {
                s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_ACTIVE;
            }
            else if (s_zs_b1_can.nm_sleep_requested != 0U)
            {
                s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_SLEEP;
            }
            else
            {
                s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_ACTIVE;
            }
        }

        zs_b1_flush_network_management();
    }
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

/* 仅在充电检测在位或电流为正时，满充、BMS故障或OBC三级故障触发停充。 */
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

/* 停充流程：条件持续10秒后请求停充；等待最多5秒，随后强制关闭充电MOS。 */
static void zs_b1_update_charger_stop(const app_bms_snapshot_t *snapshot,
                                      uint8_t detc,
                                      uint32_t elapsed_ms)
{
    bool stop_condition = zs_b1_has_charge_stop_condition(snapshot, detc);
    bool stop_wait_started = false;

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

    /* 停充条件连续成立10秒才发出请求；在达到10秒前恢复则重新计时。 */
    if (stop_condition && (s_zs_b1_can.charge_stop_commanded == 0U))
    {
        zs_b1_add_elapsed_ms(&s_zs_b1_can.charge_stop_condition_ms,
                             elapsed_ms,
                             ZS_B1_CHARGER_STOP_DELAY_MS);

        if (s_zs_b1_can.charge_stop_condition_ms >= ZS_B1_CHARGER_STOP_DELAY_MS)
        {
            s_zs_b1_can.charge_stop_commanded = 1U;
            s_zs_b1_can.charge_stop_waiting = 1U;
            s_zs_b1_can.charge_stop_wait_ms = 0U;
            stop_wait_started = true;
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
                 (((detc != 0U) &&
                   (snapshot->fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag != 0U))
                    ? 3U : 0U));
            zs_b1_set_charge_mos_force(1U);
        }
        else
        {
            /* 不把达到10秒门限的同一段elapsed重复计入5秒等待。 */
            if (!stop_wait_started)
            {
                zs_b1_add_elapsed_ms(&s_zs_b1_can.charge_stop_wait_ms,
                                     elapsed_ms,
                                     ZS_B1_CHARGER_STOP_TIMEOUT_MS);
            }
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

/* 只切换0x419周期帧的内容，不立即发送，也不重置500ms周期。 */
static void zs_b1_begin_wakeup_sequence(void)
{
    s_zs_b1_can.wakeup_frames_remaining = ZS_B1_NM_WAKEUP_FRAME_COUNT;
    s_zs_b1_can.nm_active_after_wakeup = 1U;
    s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_NONE;
}

/* 新会话从各周期的起点开始，不清零报文滚动计数。 */
static void zs_b1_start_session(uint8_t wakeup_sequence_required)
{
    s_zs_b1_can.tx_active = 1U;
    s_zs_b1_can.idle_timeout_complete = 0U;
    s_zs_b1_can.timer_network_mgmt_ms = 0U;
    s_zs_b1_can.session_idle_latched = 0U;
    s_zs_b1_can.low_current_ms = 0U;
    s_zs_b1_can.timer_status1_ms = 0U;
    s_zs_b1_can.timer_100ms_ms = 0U;
    s_zs_b1_can.timer_500ms_ms = 0U;
    s_zs_b1_can.due_100ms_mask = 0U;
    s_zs_b1_can.due_500ms_mask = 0U;
    s_zs_b1_can.next_cell_index = 0U;
    s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_NONE;
    s_zs_b1_can.wakeup_frames_remaining = 0U;
    s_zs_b1_can.nm_active_after_wakeup = 0U;
    s_zs_b1_can.nm_sleep_requested = 0U;
    s_zs_b1_can.nm_sleep_sent = 0U;
    if (wakeup_sequence_required != 0U)
    {
        zs_b1_begin_wakeup_sequence();
    }
}

/* 故障等级直接按宗申协议编码：00无故障、01一级、10二级、11三级。 */
static uint8_t zs_b1_fault_temp_level(uint16_t warning,
                                      uint16_t protection,
                                      uint16_t permanent)
{
    if (permanent != 0U) return 3U;
    if (protection != 0U) return 3U;
    if (warning != 0U) return 2U;
    return 0U;
}

/* 表内放电单体欠压：2900mV告警为二级，温度分档保护为三级。 */
static uint8_t zs_b1_fault_cell_uv_level(const t_bms_fault_flag *fault)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    if (fault_a->b1cell_deep_cuv != 0U) return 3U;
    if (fault_a->b1cell_pro_cuv1 != 0U) return 3U;
    if ((fault_a->b1cell_pro_cuv2 != 0U) || (fault_a->b1cell_war_cuv != 0U)) return 2U;
    return 0U;
}

/* 放电过流：表内一级/二级告警分别上报1/2；表内各保护档均属于三级。 */
static uint8_t zs_b1_fault_discharge_oc_level(const t_bms_fault_flag *fault,
                                              int16_t current_10ma)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *fault_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    if ((fault_a->b1pack_ocd_lock != 0U) ||
        (fault_b->b1pack_pro_ocd1 != 0U) || (fault_b->b1pack_pro_ocd2 != 0U) ||
        (fault_b->b1pack_pro_ocd3 != 0U) || (fault_b->b1pack_pro_ocd4 != 0U)) return 3U;
    if (fault_a->b1pack_war_ocd != 0U)
    {
#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
        /* 过流告警在故障层共用一位；仅在0x019组包时按已有阈值区分表内等级。 */
        if (current_10ma <= OCD2_WAR_SET_CUR_10MA) return 2U;
        if (current_10ma <= OCD1_WAR_SET_CUR_10MA) return 1U;
#endif
        return 1U;
    }
    return 0U;
}

/* 充电过流：表内23A告警为二级、25A保护为三级；保留其他现有来源的等级。 */
static uint8_t zs_b1_fault_charge_oc_level(const t_bms_fault_flag *fault)
{
    const t_charge_fault_A *fault_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *fault_b = &fault->ucharge_fault_B.tcharge_fault_B;
    if (fault_a->b1pack_pro_occ1 != 0U) return 3U;
    if (fault_a->b1pack_pro_occ2 != 0U) return 2U;
    if (fault_a->b1pack_war_occ != 0U) return 2U;
    if ((fault_a->b1pack_pro_orcc != 0U) ||
        (fault_b->b1pack_occ_lock != 0U)) return 1U;
    return 0U;
}

static uint8_t zs_b1_fault_discharge_level(const t_bms_fault_flag *fault,
                                           uint8_t discharge_pro,
                                           uint8_t discharge_oc_level)
{
    const t_discharge_fault_A *fault_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *fault_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    const t_general_fault_A *general_a = &fault->ugeneral_fault_A.tgeneral_fault_A;
    uint8_t level = 0U;

    /* 放电保护及表内三级通用放电故障优先上报三级。 */
    if ((discharge_pro != 0U) ||
        (fault_b->b1pack_shrot != 0U) ||
        (general_a->b1cell_voltage_diff_fault != 0U) ||
        (general_a->b1cell_voltage_diff_fault2 != 0U) ||
        (general_a->b1MOS_OTP != 0U) ||
        (general_a->b1DSG_MOS_faulure != 0U) ||
        (general_a->b1SCP_FUSE_fusing != 0U)) return 3U;

    /* 欠压/温度二级告警及压差/MOS过温二级告警上报二级。 */
    if ((fault_a->b1cell_war_cuv != 0U) ||
        (fault_a->b1pack_war_otd != 0U) ||
        (fault_a->b1pack_war_utd != 0U) ||
        (fault_a->b1low_soc != 0U) ||
        (fault_b->b1pack_war_cuv != 0U) ||
        (general_a->b1cell_voltage_diff_waring != 0U) ||
        (general_a->b1MOS_OTP_waring != 0U)) level = 2U;

    /* 放电过流告警同样按0x019单项等级纳入总放电故障等级。 */
    if (discharge_oc_level > level) level = discharge_oc_level;
    return level;
}

static bool zs_b1_fault_any(const app_bms_fault_image_t *fault)
{
    return ((fault->charge_pro != 0U) || (fault->discharge_pro != 0U) ||
            (fault->general_fault != 0U) || (fault->pf_fault != 0U));
}

/*
 * 0x019工作状态不把满充和充电单体/总压过压当成故障状态。
 * charge_pro和pf_fault是汇总位，因此在此处按具体故障位重建判断，
 * 保留其他充电故障及独立PF故障。
 */
static bool zs_b1_fault_any_for_work_mode(const app_bms_fault_image_t *fault)
{
    const t_charge_fault_A *charge_a = &fault->detail.ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &fault->detail.ucharge_fault_B.tcharge_fault_B;
    const t_general_fault_A *general_a = &fault->detail.ugeneral_fault_A.tgeneral_fault_A;
    bool other_charge_fault =
        ((charge_a->b1pack_pro_occ1 != 0U) ||
         (charge_a->b1pack_pro_occ2 != 0U) ||
         (charge_a->b1pack_pro_orcc != 0U) ||
         (charge_a->b1pack_pro_otc != 0U) ||
         (charge_a->b1pack_pro_utc != 0U) ||
         (charge_b->b1pack_occ_lock != 0U) ||
         (general_a->b1NTC_fault != 0U));
    bool other_pf_fault =
        ((fault->detail.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U) ||
         (general_a->b1cell_voltage_diff_fault2 != 0U) ||
         (general_a->b1SCP_FUSE_fusing != 0U) ||
         (general_a->b1CHG_MOS_failure != 0U) ||
         (general_a->b1DSG_MOS_faulure != 0U));

    return (other_charge_fault || (fault->discharge_pro != 0U) ||
            (fault->general_fault != 0U) || other_pf_fault);
}

/* 发送ID：0x019，周期100ms；工作/充电状态及电压、温度、电流和MOS等故障。 */
static bool zs_b1_send_fault(const app_bms_snapshot_t *snapshot)
{
    const t_bms_fault_flag *fault = &snapshot->fault.active.detail;
    const t_charge_fault_A *charge_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &fault->ucharge_fault_B.tcharge_fault_B;
    const t_discharge_fault_A *discharge_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *discharge_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    const t_general_fault_A *general_a = &fault->ugeneral_fault_A.tgeneral_fault_A;
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint8_t discharge_oc_level = zs_b1_fault_discharge_oc_level(
        fault, snapshot->current_10ma);
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
        fault,
        snapshot->fault.active.discharge_pro,
        discharge_oc_level);
    uint8_t work_mode = 0U;
    uint8_t charge_status = 0U;
    uint8_t power_status = 0U;

    /* Data0为校验字节，当前未实现，填0；Data1 bit0..3为0~15循环计数。 */
    data[0] = 0U;
    data[1] = (uint8_t)(s_zs_b1_can.counter_fault & 0x0FU);
    if (zs_b1_fault_any_for_work_mode(&snapshot->fault.active))
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
    /* Data1 bit4..5：0静止、1放电/回馈、2充电、3故障。 */
    data[1] |= (uint8_t)(work_mode << 4U);
    /* Data1 bit6: DETC在位为1，不在位为0。 */
    if (snapshot->signal.input.mcu_detc != 0U) data[1] |= 0x40U;

    /* 充电完成状态3要求DETC在位；单独满充标志不能判为充电完成。 */
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
    else if ((s_zs_b1_can.charge_result_valid != 0U) &&
             ((s_zs_b1_can.charge_result_status != 3U) ||
              (snapshot->signal.input.mcu_detc != 0U)))
    {
        charge_status = s_zs_b1_can.charge_result_status;
    }
    else if ((snapshot->signal.input.mcu_detc != 0U) &&
             (charge_a->b1full_charge_flag != 0U))
    {
        charge_status = 3U;
    }
    /* charge_pro汇总位包含满充标志；这里用明细故障判断，避免满充被误报为故障。 */
    else if (zs_b1_has_charge_fault(snapshot))
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
    /* Data2 bit0..1放电故障，bit2..3充电状态，bit4..5放电MOS状态。 */
    /* 充电状态：0默认、1故障、2充电中、3满充；MOS：0未知、1开、2关。 */
    data[2] = (uint8_t)((discharge_fault_level & 0x03U) |
                        ((charge_status & 0x03U) << 2U) |
                        ((power_status & 0x03U) << 4U));

    /* Data3 bit0..3：总压过压/欠压状态，来源分别是总压保护标志。 */
    if (charge_b->b1pack_pro_pov != 0U) data[3] |= 0x01U;
    if (discharge_b->b1pack_pro_cuv != 0U) data[3] |= 0x02U;
    if (charge_b->b1pack_pro_pov != 0U) data[3] |= 0x04U;
    if (discharge_b->b1pack_pro_cuv != 0U) data[3] |= 0x08U;
    /* Data3 bit4：充电单体过压为布尔指示；表内二级告警或三级保护任一触发即置1。 */
    if ((charge_a->b1cell_war_cov != 0U) ||
        (charge_a->b1cell_pro_cov != 0U) ||
        (charge_a->b1cell_pro_cov2 != 0U)) data[3] |= 0x10U;
    /* Data3 bit5/6..7为放电单体过压/充电单体欠压；表内无对应阈值，不由其他故障代置。 */
    /* Data4每两位依次为放电单体欠压、放电过温、充电过温、放电低温等级。 */
    data[4] = (uint8_t)((discharge_uv_level & 0x03U) |
                        ((discharge_temp_level & 0x03U) << 2U) |
                        ((charge_temp_level & 0x03U) << 4U) |
                        ((discharge_under_temp_level & 0x03U) << 6U));
    /* Data5每两位依次为充电低温、充电过流、放电过流和单体压差等级。 */
    data[5] = (uint8_t)((charge_under_temp_level & 0x03U) |
                        ((charge_oc_level & 0x03U) << 2U) |
                        ((discharge_oc_level & 0x03U) << 4U));
    /* Data5 bit6..7：压差告警/一级保护/二级保护依次编码为01/10/11。 */
    if (general_a->b1cell_voltage_diff_fault2 != 0U) data[5] |= 0xC0U;
    else if (general_a->b1cell_voltage_diff_fault != 0U) data[5] |= 0x80U;
    else if (general_a->b1cell_voltage_diff_waring != 0U) data[5] |= 0x40U;

    /* Data6 bit0..4未映射；bit5 MOS过温、bit6 PF故障、bit7 短路。 */
    if ((general_a->b1MOS_OTP_waring != 0U) || (general_a->b1MOS_OTP != 0U))
        data[6] |= 0x20U;
    if (snapshot->fault.active.pf_fault != 0U) data[6] |= 0x40U;
    if (discharge_b->b1pack_shrot != 0U) data[6] |= 0x80U;

    /* Data7字段30、32..34暂不实现；字段31仅报告MOS损坏PF。 */
    if ((general_a->b1CHG_MOS_failure != 0U) || (general_a->b1DSG_MOS_faulure != 0U))
        data[7] |= 0x02U;

    /* 成功提交发送后才递增4位计数；发送失败时保留原值供重试。 */
    if (!zs_b1_send(ZS_B1_ID_BMS_FAULT, data)) return false;
    s_zs_b1_can.counter_fault = (uint8_t)((s_zs_b1_can.counter_fault + 1U) & 0x0FU);
    return true;
}

/* 将SOP的0.1A单位转为报文的0.25A/bit；非正数按0处理。 */
static uint16_t zs_b1_sop_01a_to_025a(int16_t current_01a)
{
    uint32_t current_ma;
    if (current_01a <= 0) return 0U;
    current_ma = (uint32_t)(uint16_t)current_01a * 100U;
    return zs_b1_limit_u12(current_ma / 250U);
}

/* 发送ID：0x089，周期20ms；BMS状态、放电电流、SOP最大放电及回馈电流。 */
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
    bool no_feedback_allowed;
    bool data_valid = ((snapshot->data_ready != 0U) &&
                       (snapshot->cell_count != 0U) &&
                       (snapshot->valid_cells >= snapshot->cell_count));
    bool no_feedback_protection =
        ((charge_a->b1pack_pro_orcc != 0U) ||
         (charge_a->b1cell_pro_cov != 0U) ||
         (charge_a->b1cell_pro_cov2 != 0U) ||
         (charge_b->b1pack_pro_pov != 0U) ||
         (snapshot->afe_fault.cell_ov != 0U));
    /* 回馈过流、单体/总压过压或AFE单体过压时，最大回馈能力置0。 */
    max_feedback_025a = no_feedback_protection ? 0U :
        zs_b1_limit_u12(((uint32_t)snapshot->sop.u16MaxRechargeCurrent10mA * 100U) / 250U);
    /* 反馈SOP MAP折算到报文分辨率后为0时，明确上报禁止回馈。 */
    no_feedback_allowed = (max_feedback_025a == 0U);

    /* Data0 bit0数据无效、bit1 AFE/二级硬件故障、bit2放电MOS故障、bit3充电MOS故障。 */
    if (!data_valid) data[0] |= 0x01U;
    if ((general->b1AFE_fault != 0U) || (general->b1hardware_second_pro != 0U)) data[0] |= 0x02U;
    if (general->b1DSG_MOS_faulure != 0U) data[0] |= 0x04U;
    if (general->b1CHG_MOS_failure != 0U) data[0] |= 0x08U;

    /* Data0高4位：0静止、1充电、2故障、4放电、8回馈；数据无效优先报故障。 */
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
    /* Data5 bit4/5标识数据无效，bit6标识禁止回馈；Data6高4位为滚动计数。 */
    if (!data_valid) data[5] |= 0x10U | 0x20U;
    if (no_feedback_allowed) data[5] |= 0x40U;
    data[6] = (uint8_t)((s_zs_b1_can.counter_status1 & 0x0FU) << 4U);

    /* 矩阵未给出此帧校验算法，按未实现处理，校验字节置0。 */
    data[7] = 0U;
    if (!zs_b1_send(ZS_B1_ID_BMS_STATUS1, data)) return false;
    s_zs_b1_can.counter_status1 = (uint8_t)((s_zs_b1_can.counter_status1 + 1U) & 0x0FU);
    return true;
}

/* 发送ID：0x619，周期500ms；循环次数、剩余充电时间、额定电压、容量、SOC和SOH。 */
static bool zs_b1_send_status2(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    /* 满充容量有效时使用SOC估算值，否则回退到配置的额定容量；mAh转Ah。 */
    uint32_t fcc_ah = (snapshot->soc.s32battery_full_cap > 0)
        ? (uint32_t)snapshot->soc.s32battery_full_cap / 1000U
        : (uint32_t)APP_PARAM_BAT_CAPACITY / 1000U;
    /* 额定电压配置单位为mV，此字段按12V/bit编码。 */
    uint32_t rated_voltage_12v = (uint32_t)APP_PARAM_BAT_VOLTAGE / 12000U;
    uint16_t remaining_minutes = app_charge_time_get_minutes();
    if (remaining_minutes == APP_CHARGE_TIME_INVALID_MINUTES)
        remaining_minutes = 0x0FFFU; /* 协议约定：无有效估算值为12-bit全1。 */
    else if (remaining_minutes >= 0x0FFFU)
        remaining_minutes = 0x0FFEU; /* 最大有效值须避开无效码。 */

    /* Data0~1循环次数；Data2和Data3低4位剩余分钟数，Data3高4位额定电压。 */
    zs_b1_put_u16_le(data, 0U, zs_b1_limit_u16(snapshot->soc.u32cycle_index));
    data[2] = (uint8_t)(remaining_minutes & 0xFFU);
    data[3] = (uint8_t)(((remaining_minutes >> 8U) & 0x0FU) |
                        ((rated_voltage_12v & 0x0FU) << 4U));
    /* Data4/5额定及满充容量（Ah）；Data6 SOC（0.5%/bit）；Data7 SOH。 */
    data[4] = zs_b1_limit_u8((uint32_t)APP_PARAM_BAT_CAPACITY / 1000U);
    data[5] = zs_b1_limit_u8(fcc_ah);
    data[6] = zs_b1_limit_u8((uint32_t)((snapshot->soc.u8display_soc > 100U)
                                           ? 200U : snapshot->soc.u8display_soc * 2U));
    data[7] = snapshot->soc.u8display_soh;
    return zs_b1_send(ZS_B1_ID_BMS_STATUS2, data);
}

/* 发送ID：0x621，周期100ms；允许及实际充电电流、充电电压范围、停充命令和MOS状态。 */
static bool zs_b1_send_charge_control(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint16_t max_charge_005a = zs_b1_limit_charge_current_005a(
        (snapshot->sop.s16MaxChargeCurrent10mA > 0)
            ? (uint32_t)(uint16_t)snapshot->sop.s16MaxChargeCurrent10mA * 2U : 0U);
    uint32_t actual_charge_ma = (snapshot->current_business_ma > 0) ? (uint32_t)snapshot->current_business_ma : 0U;
    uint16_t actual_charge_005a = zs_b1_limit_charge_current_005a(actual_charge_ma / 50U);
    /* Data3~4最低充电电压固定40V；字段单位为0.01V。 */
    uint16_t min_charge_voltage_001v = 4000U;
    uint16_t max_charge_voltage_001v = zs_b1_limit_u16(snapshot->sop.u32MaxChargeVoltageMv / 10U);

    /* Data0~2：最大充电电流bit0..11，实际充电电流bit12..23，均为0.05A/bit。 */
    data[0] = (uint8_t)(max_charge_005a & 0xFFU);
    data[1] = (uint8_t)(((max_charge_005a >> 8U) & 0x0FU) |
                        ((actual_charge_005a & 0x0FU) << 4U));
    data[2] = (uint8_t)(actual_charge_005a >> 4U);
    /* Data3~4发送固定最低充电电压40V（4000 * 0.01V），不依赖实时总压。 */
    zs_b1_put_u16_le(data, 3U, min_charge_voltage_001v);
    /* Data5~6最高充电电压：来自SOP配置，单位0.01V，低字节在前。 */
    zs_b1_put_u16_le(data, 5U, max_charge_voltage_001v);
    /* Data7 bit0=0允许充电；仅DETC在位、充电MOS有效闭合且未停充时允许。 */
    if ((s_zs_b1_can.charge_stop_commanded != 0U) ||
        (snapshot->signal.input.mcu_detc == 0U) ||
        (snapshot->afe_status_valid == 0U) ||
        (snapshot->mos.chg_mos_state == 0U))
    {
        data[7] |= 0x01U;
    }
    /* Data7 bit1为充电MOS反馈，bit2为放电MOS反馈，bit4..7为滚动计数。 */
    if (snapshot->mos.chg_mos_state != 0U) data[7] |= 0x02U;
    if (snapshot->mos.dsg_mos_state != 0U) data[7] |= 0x04U;
    data[7] |= (uint8_t)((s_zs_b1_can.counter_charge_control & 0x0FU) << 4U);

    if (!zs_b1_send(ZS_B1_ID_BMS_CHARGE_CONTROL, data)) return false;
    s_zs_b1_can.counter_charge_control = (uint8_t)((s_zs_b1_can.counter_charge_control + 1U) & 0x0FU);
    return true;
}

/* 快照电芯下标从0开始，报文编号从1开始；无法编码时返回0xFF。 */
static uint8_t zs_b1_cell_number(uint8_t zero_based_index)
{
    return (zero_based_index < 254U) ? (uint8_t)(zero_based_index + 1U) : 0xFFU;
}

/* 发送ID：0x519，周期500ms；总压、最低/最高单体电压及其从1开始的电芯编号。 */
static bool zs_b1_send_status3(const app_bms_snapshot_t *snapshot)
{
    uint8_t data[ZS_B1_CAN_DLC] = {0U};
    uint16_t pack_voltage_001v = 0xFFFFU;
    uint16_t min_cell_mv = 0xFFFFU;
    uint16_t max_cell_mv = 0xFFFFU;

    /* 数据未就绪时电压保持0xFFFF，编号随后填0xFF。 */
    if ((snapshot->data_ready != 0U) && (snapshot->valid_cells >= snapshot->cell_count))
    {
        pack_voltage_001v = zs_b1_limit_u16(snapshot->bat_mv / 10U);
        min_cell_mv = snapshot->min_cell_mv;
        max_cell_mv = snapshot->max_cell_mv;
    }
    /* Data0~1总压（0.01V）；Data2~3最低单体（mV），Data4最低单体编号。 */
    zs_b1_put_u16_le(data, 0U, pack_voltage_001v);
    zs_b1_put_u16_le(data, 2U, min_cell_mv);
    data[4] = (min_cell_mv == 0xFFFFU) ? 0xFFU : zs_b1_cell_number(snapshot->min_cell_num);
    /* Data5~6最高单体（mV），Data7最高单体编号；多字节字段低字节在前。 */
    zs_b1_put_u16_le(data, 5U, max_cell_mv);
    data[7] = (max_cell_mv == 0xFFFFU) ? 0xFFU : zs_b1_cell_number(snapshot->max_cell_num);
    return zs_b1_send(ZS_B1_ID_BMS_STATUS3, data);
}

/* 温度编码为实际摄氏度+40；0xFF表示无效。 */
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

/* 0.1℃转整数℃时向下取整，例如-0.1℃转为-1℃。 */
static int16_t zs_b1_tenth_c_to_floor_c(int16_t temperature_0p1c)
{
    if (temperature_0p1c >= 0) return (int16_t)(temperature_0p1c / 10);
    return (int16_t)(-((-(int32_t)temperature_0p1c + 9) / 10));
}

/* 温度编号从1开始；无效或超过可编码范围时返回4位无效码0xF。 */
static uint8_t zs_b1_temperature_index_nibble(uint8_t zero_based_index, bool valid)
{
    if ((!valid) || (zero_based_index >= 14U)) return 0x0FU;
    return (uint8_t)(zero_based_index + 1U);
}

/* 发送ID：0x5A0，周期500ms；有效电芯温度平均值、最低/最高温度及编号、MOS温度。 */
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

    /* 仅将有效掩码中置位的电芯温度计入平均值。 */
    for (index = 0U; index < APP_BMS_MAX_TEMP_NUM; index++)
    {
        if ((snapshot->temp_valid_mask & (uint8_t)(1U << index)) != 0U)
        {
            temperature_sum += snapshot->temp_c[index];
            valid_count++;
        }
    }
    /* Data0平均电芯温度；Data1低/高半字节为最低/最高编号，无有效编号为0xF。 */
    data[0] = (valid_count == 0U) ? 0xFFU : zs_b1_temp_value((int16_t)(temperature_sum / valid_count));
    data[1] = (uint8_t)(zs_b1_temperature_index_nibble(snapshot->min_temp_num, valid_count != 0U) |
                        (zs_b1_temperature_index_nibble(snapshot->max_temp_num, valid_count != 0U) << 4U));
    /* Data2/3最低/最高电芯温度；Data4/5均使用当前同一MOS温度。 */
    data[2] = zs_b1_temp_value(snapshot->min_temp_c);
    data[3] = zs_b1_temp_value(snapshot->max_temp_c);
    data[4] = zs_b1_temp_value(mos_temperature_c);
    data[5] = zs_b1_temp_value(mos_temperature_c);
    /* Data6低/高半字节均为该MOS温度编号；Data7保留为0。 */
    data[6] = (uint8_t)(zs_b1_temperature_index_nibble(0U, mos_valid) |
                        (zs_b1_temperature_index_nibble(0U, mos_valid) << 4U));
    /* byte7 reserved */
    return zs_b1_send(ZS_B1_ID_BMS_STATUS4, data);
}

/* 硬件版本取字符串中前两个数字，分别作为高、低版本；单数字放低版本。 */
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

/* 软件版本提取数字作为8位无符号数；超出CAN字段范围时饱和到255。 */
static uint8_t zs_b1_parse_version_value(const char *version)
{
    uint16_t value = 0U;

    if (version == 0) return 0U;
    while (*version != '\0')
    {
        if ((*version >= '0') && (*version <= '9'))
        {
            value = (uint16_t)((value * 10U) + (uint16_t)(*version - '0'));
            if (value > 255U) return 255U;
        }
        version++;
    }
    return (uint8_t)value;
}

/* 发送ID：0x5AB，周期500ms；轮流发送一节电芯电压，同时发送硬件和软件版本。 */
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
    /* 软件版本字段按T/V映射：T为高位，V为低位，例如V01-T20发送20/1。 */
    sw_version_high = zs_b1_parse_version_value(APP_PARAM_PACKAGE_T_VERSION);
    sw_version_low = zs_b1_parse_version_value(APP_PARAM_PACKAGE_V_VERSION);

    /* Data0为电芯编号，Data1~2为该电芯电压（mV，小端）；无效时分别为FF/FFFF。 */
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
    /* Data3保留；Data4/5硬件高/低版本，Data6/7软件T高位/V低位。 */
    data[4] = hw_version_high;
    data[5] = hw_version_low;
    data[6] = sw_version_high;
    data[7] = sw_version_low;

    /* 成功发送且数据有效后才轮到下一节，发送到末节后从第1节重新开始。 */
    if (!zs_b1_send(ZS_B1_ID_BMS_STATUS5, data)) return false;
    if ((snapshot->data_ready != 0U) && (snapshot->valid_cells >= snapshot->cell_count) &&
        (snapshot->cell_count != 0U))
    {
        s_zs_b1_can.next_cell_index++;
        if (s_zs_b1_can.next_cell_index >= snapshot->cell_count) s_zs_b1_can.next_cell_index = 0U;
    }
    return true;
}

static void zs_b1_tick_timer(uint16_t *timer, uint16_t period, uint32_t elapsed_ms)
{
    if (*timer >= period) return;
    if (elapsed_ms >= ((uint32_t)period - *timer))
        *timer = period;
    else
        *timer = (uint16_t)(*timer + elapsed_ms);
}

/* 同组报文逐个发送；失败的位保留到后续task重试，整组成功才重启周期。 */
static void zs_b1_schedule_periodic(const app_bms_snapshot_t *snapshot,
                                    uint32_t elapsed_ms)
{
    /* 20ms组：只包含0x089；提交成功后清零计时。 */
    zs_b1_tick_timer(&s_zs_b1_can.timer_status1_ms,
                     ZS_B1_PERIOD_STATUS1_MS,
                     elapsed_ms);
    if (s_zs_b1_can.timer_status1_ms >= ZS_B1_PERIOD_STATUS1_MS)
    {
        if (zs_b1_send_status1(snapshot)) s_zs_b1_can.timer_status1_ms = 0U;
    }

    /* 100ms组：位0对应0x019，位1对应0x621；成功发送后清除对应位。 */
    if (s_zs_b1_can.timer_100ms_ms < ZS_B1_PERIOD_100MS)
    {
        zs_b1_tick_timer(&s_zs_b1_can.timer_100ms_ms,
                         ZS_B1_PERIOD_100MS,
                         elapsed_ms);
        if (s_zs_b1_can.timer_100ms_ms >= ZS_B1_PERIOD_100MS)
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

    /* 500ms状态组：位0~3对应0x619、0x519、0x5A0、0x5AB。 */
    if (s_zs_b1_can.timer_500ms_ms < ZS_B1_PERIOD_500MS)
    {
        zs_b1_tick_timer(&s_zs_b1_can.timer_500ms_ms,
                         ZS_B1_PERIOD_500MS,
                         elapsed_ms);
        if (s_zs_b1_can.timer_500ms_ms >= ZS_B1_PERIOD_500MS)
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

    /* 0x419使用独立500ms周期；只读取当前NM状态决定Data0内容。 */
    zs_b1_schedule_network_management(elapsed_ms);
}

void zs_b1_can_protocol_init(void)
{
    memset(&s_zs_b1_can, 0, sizeof(s_zs_b1_can));
    s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_NONE;
}

void zs_b1_can_protocol_task(bool can_transport_active)
{
    app_bms_snapshot_t snapshot;
    uint32_t now_ms;
    uint32_t elapsed_ms;
    uint8_t detd;
    uint8_t detc;
    uint8_t key_present;
    bool ignition_rising;
    bool charge_enable_changed;
    bool wakeup_event;
    uint8_t has_business_current;
    bool idle_without_key_or_current;
    bool charge_context;
    bool charge_event;

    /* 用TIM6时间戳计算真实经过时间，不假设任务每次调用恰好间隔1ms。 */
    now_ms = d_timer_get_ms();
    if (s_zs_b1_can.task_time_valid != 0U)
        elapsed_ms = now_ms - s_zs_b1_can.last_task_ms;
    else
        elapsed_ms = 0U;
    s_zs_b1_can.last_task_ms = now_ms;
    s_zs_b1_can.task_time_valid = 1U;

    /* 统一读取本次任务的快照；KEY有效期由接收模块维护。 */
    app_bms_snapshot_read(&snapshot);
    detd = (snapshot.signal.input.mcu_detd != 0U) ? 1U : 0U;
    detc = (snapshot.signal.input.mcu_detc != 0U) ? 1U : 0U;
    key_present = (snapshot.signal.input.mcu_key_present != 0U) ? 1U : 0U;
    ignition_rising = ((detd != 0U) &&
                       (s_zs_b1_can.previous_detd == 0U));
    charge_enable_changed = (detc != s_zs_b1_can.previous_detc);
    /* DETD上升沿为IGN ON；DETC插入或拔出都属于充电使能插拔事件。 */
    wakeup_event = (ignition_rising || charge_enable_changed);
    idle_without_key_or_current = ((key_present == 0U) &&
                                   (wakeup_event == false) &&
                                   (can_transport_active == false) &&
                                   (snapshot.current_10ma == 0));
    s_zs_b1_can.previous_detd = detd;
    s_zs_b1_can.previous_detc = detc;
    has_business_current = (snapshot.current_10ma != 0) ? 1U : 0U;
    charge_context = ((detc != 0U) || (snapshot.current_10ma > 0));
    zs_b1_update_charger_stop(&snapshot, detc, elapsed_ms);
    charge_event = (charge_context &&
                    ((snapshot.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag != 0U) ||
                     zs_b1_has_charge_fault(&snapshot) ||
                     (s_zs_b1_can.charger_fault_report_latched != 0U)));

    /* KEY在位期间持续发送，并清零待停发计时；拔掉KEY后重新完整等待20秒。 */
    if (key_present != 0U)
    {
        s_zs_b1_can.session_idle_latched = 0U;
        s_zs_b1_can.idle_timeout_complete = 0U;
        s_zs_b1_can.low_current_ms = 0U;
    }
    /* IGN/充电使能状态变化或非零业务电流解除停发锁存。 */
    if ((wakeup_event != false) || (has_business_current != 0U) ||
        (can_transport_active != false))
    {
        s_zs_b1_can.session_idle_latched = 0U;
        s_zs_b1_can.idle_timeout_complete = 0U;
    }

    /* KEY、IGN/充电使能变化、业务电流或充电事件可触发发送会话。 */
    if ((s_zs_b1_can.tx_active == 0U) &&
        (s_zs_b1_can.session_idle_latched == 0U) &&
        ((key_present != 0U) || wakeup_event ||
         (has_business_current != 0U) ||
         (can_transport_active != false) || charge_event))
    {
        zs_b1_start_session(wakeup_event ? 1U : 0U);
        /* 新会话计时器刚清零，本次调用前的间隔不属于发送会话。 */
        elapsed_ms = 0U;
    }
    else if ((s_zs_b1_can.tx_active != 0U) &&
             (wakeup_event != false) &&
             (s_zs_b1_can.wakeup_frames_remaining == 0U))
    {
        /* 会话已运行时遇到新的IGN/充电使能事件，也启动11帧Wakeup；已有序列不重复重启。 */
        zs_b1_begin_wakeup_sequence();
    }

    if (s_zs_b1_can.tx_active == 0U)
    {
        /* 空闲20秒到期后只补发已经到期的0x419 Sleep帧，不恢复其它周期报文。 */
        if (s_zs_b1_can.due_nm_state != ZS_B1_NM_STATE_NONE)
            zs_b1_flush_network_management();
        return;
    }

    /* KEY缺失后才累计无电流时间；IGN/DETC变化或电流活动时重新计时并取消Sleep请求。 */
    if (idle_without_key_or_current == false)
    {
        s_zs_b1_can.low_current_ms = 0U;
        s_zs_b1_can.nm_sleep_requested = 0U;
        s_zs_b1_can.nm_sleep_sent = 0U;
    }
    else if (s_zs_b1_can.low_current_ms < APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS)
    {
        zs_b1_add_elapsed_ms(&s_zs_b1_can.low_current_ms,
                             elapsed_ms,
                             APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS);
    }

    /* 无KEY且电流为零的等待窗口一开始，就让0x419在原500ms周期发送Sleep。 */
    if (idle_without_key_or_current != false)
    {
        if (s_zs_b1_can.nm_sleep_requested == 0U)
        {
            /* 新空闲窗口需要重新确认至少一次Sleep帧已成功提交。 */
            s_zs_b1_can.nm_sleep_sent = 0U;
        }
        s_zs_b1_can.nm_sleep_requested = 1U;

        /* 普通Active帧若因CAN忙尚未提交，替换为当前应发送的Sleep状态。 */
        if ((s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_ACTIVE) &&
            (s_zs_b1_can.wakeup_frames_remaining == 0U) &&
            (s_zs_b1_can.nm_active_after_wakeup == 0U))
        {
            s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_SLEEP;
        }
    }

    /* KEY/电流恢复时，撤销待发Sleep；之后由Active或Wakeup流程恢复通信。 */
    if ((s_zs_b1_can.nm_sleep_requested == 0U) &&
        (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_SLEEP))
    {
        s_zs_b1_can.due_nm_state = ZS_B1_NM_STATE_ACTIVE;
    }

    if (s_zs_b1_can.low_current_ms >= APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS)
    {
        s_zs_b1_can.idle_timeout_complete = 1U;
        /* KEY缺失且无电流连续达到20秒后，停止其余周期报文；0x419仍按500ms节拍重试Sleep。 */
        if (key_present == 0U)
        {
            s_zs_b1_can.due_100ms_mask = 0U;
            s_zs_b1_can.due_500ms_mask = 0U;
            s_zs_b1_can.nm_sleep_requested = 1U;
            zs_b1_schedule_network_management(elapsed_ms);
            if ((s_zs_b1_can.nm_sleep_sent != 0U) &&
                (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_NONE))
            {
                s_zs_b1_can.tx_active = 0U;
                s_zs_b1_can.session_idle_latched = 1U;
            }
            return;
        }
    }

    zs_b1_schedule_periodic(&snapshot, elapsed_ms);
}

bool zs_b1_can_protocol_allows_sleep(void)
{
    return ((s_zs_b1_can.tx_active == 0U) &&
            (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_NONE));
}

/* 只有周期发送已经停下并锁存空闲后，才算总线静默；Sleep准备期不算。 */
bool zs_b1_can_protocol_is_silent(void)
{
    return ((s_zs_b1_can.tx_active == 0U) &&
            (s_zs_b1_can.session_idle_latched != 0U) &&
            (s_zs_b1_can.nm_sleep_requested != 0U) &&
            (s_zs_b1_can.due_nm_state == ZS_B1_NM_STATE_NONE));
}

bool zs_b1_can_protocol_idle_timeout_complete(void)
{
    return ((s_zs_b1_can.idle_timeout_complete != 0U) &&
            (s_zs_b1_can.tx_active == 0U));
}

void zs_b1_can_protocol_notify_wakeup(void)
{
    zs_b1_start_session(1U);
    /* 唤醒启动新会话，首次任务调用只建立时间基线，不计入休眠前的间隔。 */
    s_zs_b1_can.task_time_valid = 0U;
}
