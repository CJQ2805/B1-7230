/*
 * 文件作用：
 *   APP层BMS故障处理模块，负责软件保护判断、AFE硬件故障合并和故障变化检测。
 *
 * 主要用途：
 *   1. 根据电压、电流、温度和AFE状态置位或清除故障。
 *   2. 将软件故障和AFE硬件保护合并成最终故障图。
 *   3. 输出充电禁止、放电禁止、通用故障、PF故障等汇总状态。
 *
 * 适用范围：
 *   适用于当前20串BMS保护策略。模块只读取统一业务快照和硬件动作适配接口，不直接绑定具体AFE
 *   型号、寄存器结构或板级引脚。
 *
 * 调用关系：
 *   BMS_Task以10ms周期调用 app_bms_fault_task()；MOS、LOG、通信模块读取本模块公开故障对象或接口。
 *
 * 注意事项：
 *   本模块是安全核心，阈值、延时和故障合并关系必须保持可追溯；修改后必须上板验证保护动作。
 */
#include "app_bms_fault.h"
#include "app_bms_fault_config.h"
#include "app_bms_fault_config_validate.h"
#include "App/app_bms_cuv_temp_policy.h"
#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "App/app_input_signals.h"
#include "App/app_flash_map.h"
#include "App/app_hw_apply.h"
#include "app_log_manage.h"
#include "Driver/d_flash.h"
#include "Driver/d_timer.h"
#include "RTT/SEGGER_RTT.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include <string.h>

/* cond 连续满足 time 后置位 field，用来过滤瞬时抖动。 */
#define BMS_DELAY_SET(cond, dly, time, field)                 \
    do                                                        \
    {                                                         \
        if (d_timer_delay_check((bool)(cond), &(dly), (time)) == Ok)    \
        {                                                     \
            (field) = 1U;                                     \
        }                                                     \
    } while (0)

/* cond 连续满足 time 后清除 field，置位和恢复使用独立计时器。 */
/* 只有故障位已经置位时才允许恢复计时；未置位时复位计时器，避免旧计时残留。 */
#define BMS_DELAY_CLEAR(cond, dly, time, field)               \
    do                                                        \
    {                                                         \
        if ((field) != 0U)                                    \
        {                                                     \
            if (d_timer_delay_check((bool)(cond), &(dly), (time)) == Ok) \
            {                                                 \
                (field) = 0U;                                 \
            }                                                 \
        }                                                     \
        else                                                  \
        {                                                     \
            (void)d_timer_delay_check(false, &(dly), (time)); \
        }                                                     \
    } while (0)

/* 软件保护关闭时，清掉软件保护图；最终故障仍会在 app_bms_fault_update_afe 中合并 AFE 硬件保护。 */
#define BMS_SW_FAULT_CLEAR(field)                             \
    do                                                        \
    {                                                         \
        (field) = 0U;                                         \
    } while (0)

/* 同一项保护的置位/恢复计时器成对声明；外层#if仍由对应功能开关独立控制。 */
#define BMS_DECLARE_STATIC_VARIABLE(type, name) static type name
#define BMS_DECLARE_DELAY_VARIABLE(name)         BMS_DECLARE_STATIC_VARIABLE(u32TimDly_t, name)
#define BMS_DECLARE_DELAY_PAIR(prefix)           \
    BMS_DECLARE_DELAY_VARIABLE(prefix##_set);     \
    BMS_DECLARE_DELAY_VARIABLE(prefix##_recover)

/* App database固定PF页：shutdown、复位前、升级前保存，上电时恢复。 */
#define BMS_PF_SHUTDOWN_PAGE_ADDR        APP_FLASH_MAP_DB_PF_ADDR
#define BMS_PF_SHUTDOWN_RECORD_ADDR      BMS_PF_SHUTDOWN_PAGE_ADDR
#define BMS_PF_SHUTDOWN_TAG              APP_FLASH_TAG_PF
#define BMS_AFE_CLEAR_RETRY_TIME         TMR0_DELAY_SEC(1U) /* AFE清除后仍置位时，至少间隔1秒再重试。 */

/* PF锁存来源位图：只保存真正需要掉电保留的PF来源，不保存普通告警和可恢复保护。 */
#define BMS_PF_MASK_CELL_DEEP_CUV        ((uint32_t)1UL << 0)
#define BMS_PF_MASK_CELL_COV2            ((uint32_t)1UL << 1)
#define BMS_PF_MASK_CELL_DIFF            ((uint32_t)1UL << 2)
#define BMS_PF_MASK_SCP_FUSE             ((uint32_t)1UL << 3)
#define BMS_PF_MASK_CHG_MOS_FAIL         ((uint32_t)1UL << 4)
#define BMS_PF_MASK_DSG_MOS_FAIL         ((uint32_t)1UL << 5)

typedef struct
{
    uint8_t cell_count;
    uint8_t valid_cells;
    uint16_t max_cell_mv;
    uint16_t min_cell_mv;
    uint16_t cell_gap_mv;
    uint32_t pack_voltage_mv;
    int32_t current_ma;
    int16_t current_10ma;
    int16_t max_cell_temp_c; /* 所有有效电芯温度中的最大值，过温保护使用。 */
    int16_t min_cell_temp_c; /* 所有有效电芯温度中的最小值，欠温保护使用。 */
    bool temp_data_ready;    /* 至少有一路有效电芯温度。 */
    int16_t mos_temp_0p1c;
    bool afe_status_valid;
    bool chg_mos_on;
    bool dsg_mos_on;
    bool afe_iic_watchdog;
    bool afe_cell_ov;
    bool afe_cell_uv;
    bool afe_chg_oc1;
    bool afe_chg_oc2;
    bool afe_dsg_oc1;
    bool afe_dsg_oc2;
    bool afe_short_circuit;
    bool afe_ntc_iic_fault;
    bool mos_temp_valid;
    uint8_t ntc_abnormal_count;
    bool ntc_fault;
    bool ntc_idle;
    bool data_ready;
    bool charging;             /* 正向电流，仅供电流方向相关的恢复条件使用。 */
    bool discharging;          /* 负向电流。 */
    uint8_t bms_work_status;   /* 充电保护/告警置位使用统一工作状态。 */
    bool detd_present;
    bool detc_present;
} bms_fault_data_t;

#pragma pack(push, 1)
typedef struct
{
    uint16_t tag;       /* Flash中可直接识别为“PF”。 */
    uint32_t pf_mask;   /* 关机前锁存的PF来源位图。 */
    uint32_t u32overcharge_count;    /* 过充保护事件次数；旧记录中该字段为空白。 */
    uint32_t u32overdischarge_count; /* 过放保护事件次数；旧记录中该字段为空白。 */
} bms_fault_pf_shutdown_record_t;
#pragma pack(pop)

struct _app_bms_fault_t gt_app_bms_fault;

void app_bms_fault_snapshot_read(struct _app_bms_fault_t *snapshot)
{
    if (snapshot == NULL)
    {
        return;
    }

    taskENTER_CRITICAL();
    *snapshot = gt_app_bms_fault;
    taskEXIT_CRITICAL();
}
uint16_t s_bms_ocd_recover_count; /* 放电过流和短路保护触发累计次数，用于放电过流锁定判断。 */
static uint8_t s_bms_short_event_previous_active; /* 短路事件边沿基线，避免故障位持续置位时重复计数。 */
static uint16_t s_bms_occ_recover_count; /* 充电过流保护累计恢复次数，用于调试观察锁定条件。 */
static bms_fault_data_t s_bms_fault_last_data; /* 最近一次故障任务采集的数据，便于反查判断输入。 */
static uint8_t s_bms_fault_last_clear_mask; /* 最近一次准备写入AFE的清除掩码。 */
static uint8_t s_bms_fault_clear_wait_mask; /* 最近一次AFE清除失败后保留的等待掩码。 */
static uint8_t s_bms_efuse_cmd; /* 当前二级熔断输出命令，1表示输出有效。 */
static uint8_t s_bms_efuse_initialized;
static uint8_t s_bms_pf_record_dirty; /* 电压保护次数变化后，等待低优先级任务写入PF页。 */
static uint8_t s_bms_overcharge_event_previous_active;
static uint8_t s_bms_overdischarge_event_previous_active;

/* 仅在计数或锁状态变化时打印，便于定位过流次数没有累计的原因。 */
static void bms_fault_rtt_ocd_count(const char *event, int16_t current_10ma)
{
    SEGGER_RTT_printf(0, "OCD %s t=%lu count=%u lock=%u I10=%d\r\n",
                      event,
                      (unsigned long)d_timer_get_ms(),
                      (unsigned int)s_bms_ocd_recover_count,
                      (unsigned int)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock,
                      (int)current_10ma);
}

static void bms_fault_rtt_ocd_reset(const char *reason, uint16_t old_count, uint8_t old_lock)
{
    if ((old_count != 0U) || (old_lock != 0U))
    {
        SEGGER_RTT_printf(0, "OCD reset=%s t=%lu count=%u->0 lock=%u I10=%d\r\n",
                          reason,
                          (unsigned long)d_timer_get_ms(),
                          (unsigned int)old_count,
                          (unsigned int)old_lock,
                          (int)s_bms_fault_last_data.current_10ma);
    }
}

static void bms_fault_rtt_ocd_warning(const char *event)
{
    SEGGER_RTT_printf(0, "OCD WARN %s t=%lu I10=%d DM=%u LD=%u\r\n",
                      event,
                      (unsigned long)d_timer_get_ms(),
                      (int)s_bms_fault_last_data.current_10ma,
                      (unsigned int)s_bms_fault_last_data.dsg_mos_on,
                      (unsigned int)app_hw_apply_load_detect_level());
}

static uint8_t bms_fault_is_overcharge_active(const t_bms_fault_flag *fault)
{
    return ((fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov != 0U) ||
            (fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 != 0U) ||
            (fault->ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov != 0U)) ? 1U : 0U;
}

static uint8_t bms_fault_is_overdischarge_active(const t_bms_fault_flag *fault)
{
    return ((fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 != 0U) ||
            (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 != 0U) ||
            (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U) ||
            (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv != 0U)) ? 1U : 0U;
}

static bool bms_fault_pf_record_is_valid(const bms_fault_pf_shutdown_record_t *record)
{
    const uint32_t u32AllowedMask = BMS_PF_MASK_CELL_DEEP_CUV |
                                    BMS_PF_MASK_CELL_COV2 |
                                    BMS_PF_MASK_CELL_DIFF |
                                    BMS_PF_MASK_SCP_FUSE |
                                    BMS_PF_MASK_CHG_MOS_FAIL |
                                    BMS_PF_MASK_DSG_MOS_FAIL;
    uint32_t u32PfMask;
    uint32_t u32OverchargeCount;
    uint32_t u32OverdischargeCount;

    if (record == 0)
    {
        return false;
    }

    if (record->tag != BMS_PF_SHUTDOWN_TAG)
    {
        return false;
    }

    memcpy(&u32PfMask,
           ((const uint8_t *)record) + sizeof(record->tag),
           sizeof(u32PfMask));
    memcpy(&u32OverchargeCount,
           ((const uint8_t *)record) + sizeof(record->tag) + sizeof(u32PfMask),
           sizeof(u32OverchargeCount));
    memcpy(&u32OverdischargeCount,
           ((const uint8_t *)record) + sizeof(record->tag) + sizeof(u32PfMask) + sizeof(u32OverchargeCount),
           sizeof(u32OverdischargeCount));

    return (((u32PfMask & ~u32AllowedMask) == 0UL) &&
            ((u32PfMask != 0UL) ||
             ((u32OverchargeCount != 0xFFFFFFFFUL) && (u32OverchargeCount != 0UL)) ||
             ((u32OverdischargeCount != 0xFFFFFFFFUL) && (u32OverdischargeCount != 0UL))));
}

static uint32_t bms_fault_collect_pf_mask(const t_bms_fault_flag *fault)
{
    uint32_t pf_mask = 0UL;

    if (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U)
    {
        pf_mask |= BMS_PF_MASK_CELL_DEEP_CUV;
    }

    if (fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 != 0U)
    {
        pf_mask |= BMS_PF_MASK_CELL_COV2;
    }

    if (fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 != 0U)
    {
        /* 保持旧PF位图兼容：bit2原本即表示压差PF，现在仅由锁存的二级保护使用。 */
        pf_mask |= BMS_PF_MASK_CELL_DIFF;
    }

    if (fault->ugeneral_fault_A.tgeneral_fault_A.b1SCP_FUSE_fusing != 0U)
    {
        pf_mask |= BMS_PF_MASK_SCP_FUSE;
    }

    if (fault->ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure != 0U)
    {
        pf_mask |= BMS_PF_MASK_CHG_MOS_FAIL;
    }

    if (fault->ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure != 0U)
    {
        pf_mask |= BMS_PF_MASK_DSG_MOS_FAIL;
    }

    return pf_mask;
}

static void bms_fault_restore_pf_mask(uint32_t pf_mask)
{
    /* 上电只恢复PF来源位，普通告警和可恢复保护仍由10ms故障任务根据实时数据重新判断。 */
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv =
        ((pf_mask & BMS_PF_MASK_CELL_DEEP_CUV) != 0UL) ? 1U : 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 =
        ((pf_mask & BMS_PF_MASK_CELL_COV2) != 0UL) ? 1U : 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 =
        ((pf_mask & BMS_PF_MASK_CELL_DIFF) != 0UL) ? 1U : 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1SCP_FUSE_fusing =
        ((pf_mask & BMS_PF_MASK_SCP_FUSE) != 0UL) ? 1U : 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure =
        ((pf_mask & BMS_PF_MASK_CHG_MOS_FAIL) != 0UL) ? 1U : 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure =
        ((pf_mask & BMS_PF_MASK_DSG_MOS_FAIL) != 0UL) ? 1U : 0U;
}

static uint32_t bms_pack_threshold(uint16_t cell_threshold_mv, uint8_t cell_count)
{
    return (uint32_t)cell_threshold_mv * (uint32_t)cell_count;
}

static void bms_fault_collect_data(bms_fault_data_t *ptFaultData)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t u8CellIndex;
    uint32_t u32PackVoltageSumMv = 0UL;
#if (BMS_PROTECT_NTC_FAULT_ENABLE != 0U)
    int32_t s32CurrentAbsMa;
#endif

    app_bms_snapshot_read(&tBmsSnapshot);
    memset(ptFaultData, 0, sizeof(*ptFaultData));
    ptFaultData->cell_count  = tBmsSnapshot.cell_count;
    ptFaultData->min_cell_mv = 0xFFFFU;

    for (u8CellIndex = 0U; u8CellIndex < ptFaultData->cell_count; u8CellIndex++)
    {
        uint16_t u16CellVoltageMv = tBmsSnapshot.cell_mv[u8CellIndex];

        if ((u16CellVoltageMv == 0U) || (u16CellVoltageMv > 6000U))
        {
            continue;
        }

        u32PackVoltageSumMv += u16CellVoltageMv;
        ptFaultData->valid_cells++;

        if (u16CellVoltageMv > ptFaultData->max_cell_mv)
        {
            ptFaultData->max_cell_mv = u16CellVoltageMv;
        }

        if (u16CellVoltageMv < ptFaultData->min_cell_mv)
        {
            ptFaultData->min_cell_mv = u16CellVoltageMv;
        }
    }

    ptFaultData->data_ready = ((tBmsSnapshot.data_ready != 0U) &&
                               (ptFaultData->cell_count > 0U) &&
                               (ptFaultData->valid_cells == ptFaultData->cell_count));
    if (ptFaultData->data_ready == false)
    {
        ptFaultData->min_cell_mv = 0U;
    }

    ptFaultData->cell_gap_mv = (uint16_t)(ptFaultData->max_cell_mv - ptFaultData->min_cell_mv);
    ptFaultData->pack_voltage_mv = (tBmsSnapshot.vbat_mv != 0UL) ?
                                   tBmsSnapshot.vbat_mv : u32PackVoltageSumMv;
    ptFaultData->current_ma = tBmsSnapshot.current_ma;
    ptFaultData->current_10ma = tBmsSnapshot.current_10ma;
    /* 温度保护统一使用快照层已经整理好的最大值和最小值。 */
    ptFaultData->max_cell_temp_c = tBmsSnapshot.max_temp_c;
    ptFaultData->min_cell_temp_c = tBmsSnapshot.min_temp_c;
    ptFaultData->temp_data_ready = (tBmsSnapshot.temp_count > 0U);
    ptFaultData->mos_temp_0p1c = tBmsSnapshot.ntc.mos_temp_0p1c;
    ptFaultData->afe_status_valid = (tBmsSnapshot.afe_status_valid != 0U);
    ptFaultData->chg_mos_on = (tBmsSnapshot.mos.chg_mos_state != 0U);
    ptFaultData->dsg_mos_on = (tBmsSnapshot.mos.dsg_mos_state != 0U);
    ptFaultData->afe_iic_watchdog = (tBmsSnapshot.afe_fault.iic_watchdog != 0U);
    ptFaultData->afe_cell_ov = (tBmsSnapshot.afe_fault.cell_ov != 0U);
    ptFaultData->afe_cell_uv = (tBmsSnapshot.afe_fault.cell_uv != 0U);
    ptFaultData->afe_chg_oc1 = (tBmsSnapshot.afe_fault.chg_oc1 != 0U);
    ptFaultData->afe_chg_oc2 = (tBmsSnapshot.afe_fault.chg_oc2 != 0U);
    ptFaultData->afe_dsg_oc1 = (tBmsSnapshot.afe_fault.dsg_oc1 != 0U);
    ptFaultData->afe_dsg_oc2 = (tBmsSnapshot.afe_fault.dsg_oc2 != 0U);
    ptFaultData->afe_short_circuit = (tBmsSnapshot.afe_fault.short_circuit != 0U);
    ptFaultData->afe_ntc_iic_fault = (tBmsSnapshot.afe_fault.ntc_iic_fault != 0U);
    ptFaultData->mos_temp_valid = ((tBmsSnapshot.ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U);
    /* NTC-Fault只依据5路NTC电气异常数量，AFE_NTC的IIC失败不计入这里。 */
    ptFaultData->ntc_abnormal_count = tBmsSnapshot.ntc_abnormal_count;
    ptFaultData->ntc_fault = (ptFaultData->ntc_abnormal_count >= BMS_NTC_FAULT_MIN_COUNT);
    /* 电流零点死区已经在AFE采集源头处理，这里只按正负判断当前方向。 */
    ptFaultData->charging = (ptFaultData->current_10ma > 0);
    ptFaultData->discharging = (ptFaultData->current_10ma < 0);
    ptFaultData->bms_work_status = tBmsSnapshot.bms_work_status;
    ptFaultData->detd_present = (tBmsSnapshot.signal.input.mcu_detd != 0U);
    ptFaultData->detc_present = (tBmsSnapshot.signal.input.mcu_detc != 0U);

#if (BMS_PROTECT_NTC_FAULT_ENABLE != 0U)
    /* NTC故障只有在电池空闲时才允许进入保护，通信仍然实时上报异常数量。 */
    s32CurrentAbsMa = ptFaultData->current_ma;
    if (s32CurrentAbsMa < 0)
    {
        s32CurrentAbsMa = -s32CurrentAbsMa;
    }
    ptFaultData->ntc_idle = ((s32CurrentAbsMa <= BMS_NTC_PROTECT_IDLE_CURRENT_MA) &&
                             (ptFaultData->charging == false) &&
                             (ptFaultData->discharging == false));
#endif
}

uint8_t app_bms_fault_data_ready(void)
{
    bms_fault_data_t data;

    bms_fault_collect_data(&data);

    return (uint8_t)data.data_ready;
}

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U) && (BMS_PROTECT_AFE_FAULT_ENABLE != 0U)
static void bms_fault_update_afe_fault(const bms_fault_data_t *data)
{
    BMS_DECLARE_DELAY_VARIABLE(dly_set_afe_fault);
    BMS_DECLARE_DELAY_VARIABLE(dly_recover_afe_fault);
    bool afe_fault_condition;

    afe_fault_condition = ((data->data_ready == false) ||
                           data->afe_iic_watchdog ||
                           data->afe_ntc_iic_fault);

    /* AFE_fault 置位：AFE数据未齐、IIC看门狗报警或NTC寄存器读取失败持续3s。 */
    BMS_DELAY_SET(afe_fault_condition,
                  dly_set_afe_fault,
                  AFE_FAULT_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault);

    /* AFE_fault 清除：数据恢复且AFE所有IIC状态正常持续10ms。 */
    BMS_DELAY_CLEAR((afe_fault_condition == false),
                    dly_recover_afe_fault,
                    AFE_FAULT_RECOVER_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault);
}
#endif

/*
 * AFE故障恢复计时器：只有对应AFE故障当前确实存在，且恢复条件连续满足，才允许产生清除请求。
 * 清除请求成功后由等待掩码防止在下一次状态读取前重复写IIC；本地gt_hsi_afe.status不在这里改写。
 */
static bool bms_fault_afe_recovery_ready(bool afe_fault,
                                          bool recovery_condition,
                                          uint8_t afe_mask,
                                          uint8_t clear_wait_mask,
                                          u32TimDly_t *delay,
                                          uint32_t recover_time)
{
    if ((afe_fault == false) || ((clear_wait_mask & afe_mask) != 0U))
    {
        (void)d_timer_delay_check(false, delay, recover_time);
        return false;
    }

    return (d_timer_delay_check(recovery_condition, delay, recover_time) == Ok);
}

/*
 * 独立处理DVC1124 R0故障位恢复。
 * 这条链路不受BMS_SW_PROTECT_ENABLE及各软件保护宏影响，因为AFE硬件保护本身
 * 可能已经发生，软件只负责在实时恢复条件满足后向AFE写对应的清除位。
 */
static void bms_fault_update_afe_recovery(const bms_fault_data_t *data)
{
    BMS_DECLARE_DELAY_VARIABLE(dly_cov_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_cuv_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_occ1_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_occ2_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd1_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd2_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_scd_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_iwf_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_afe_clear_retry);
    static uint8_t clear_wait_mask;
    uint8_t clear_mask = APP_HW_AFE_CLEAR_NONE;
    bool afe_status_ready;

    if (data == 0)
    {
        return;
    }

    s_bms_fault_last_clear_mask = APP_HW_AFE_CLEAR_NONE;

    afe_status_ready = (data->data_ready && data->afe_status_valid);

    /* 只有读到对应AFE故障位为0，才立即结束该位的等待。故障位仍为1时，
     * 保留等待掩码，并通过1秒重试计时器重新允许清除，避免永久等待或10ms重复刷写。 */
    if (afe_status_ready)
    {
        if (data->afe_cell_ov == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_COV); }
        if (data->afe_cell_uv == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_CUV); }
        if (data->afe_chg_oc1 == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_OCC1); }
        if (data->afe_chg_oc2 == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_OCC2); }
        if (data->afe_dsg_oc1 == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_OCD1); }
        if (data->afe_dsg_oc2 == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_OCD2); }
        if (data->afe_short_circuit == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_SCD); }
        if (data->afe_iic_watchdog == false) { clear_wait_mask &= (uint8_t)(~APP_HW_AFE_CLEAR_IWF); }

        if (clear_wait_mask != APP_HW_AFE_CLEAR_NONE)
        {
            if (d_timer_delay_check(true,
                                    &dly_afe_clear_retry,
                                    BMS_AFE_CLEAR_RETRY_TIME) == Ok)
            {
                /* 到达重试间隔后释放等待掩码，由各故障对应的恢复逻辑重新发起清除。 */
                clear_wait_mask = APP_HW_AFE_CLEAR_NONE;
            }
        }
        else
        {
            (void)d_timer_delay_check(false,
                                      &dly_afe_clear_retry,
                                      BMS_AFE_CLEAR_RETRY_TIME);
        }
    }
    else
    {
        /* 没有有效AFE状态时不计重试时间，避免用旧状态盲目重发清除命令。 */
        (void)d_timer_delay_check(false,
                                  &dly_afe_clear_retry,
                                  BMS_AFE_CLEAR_RETRY_TIME);
    }

    /* AFE过压没有级别信息，按软件一级保护的恢复阈值和时间处理。 */
    if (bms_fault_afe_recovery_ready(data->afe_cell_ov,
                                     afe_status_ready &&
                                     ((data->max_cell_mv <= AFE_CLEAR_COV_RECOVER_VOLTAGE) || data->discharging),
                                     APP_HW_AFE_CLEAR_COV,
                                     clear_wait_mask,
                                     &dly_cov_recover,
                                     AFE_CLEAR_COV_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_COV;
    }

    /* AFE欠压按软件二级欠压保护的恢复阈值和时间处理，充电状态也允许恢复。 */
    if (bms_fault_afe_recovery_ready(data->afe_cell_uv,
                                     afe_status_ready &&
                                     ((data->min_cell_mv >= AFE_CLEAR_CUV_RECOVER_VOLTAGE) || data->charging),
                                     APP_HW_AFE_CLEAR_CUV,
                                     clear_wait_mask,
                                     &dly_cuv_recover,
                                     AFE_CLEAR_CUV_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_CUV;
    }

    /* 充电过流恢复：无充电电流，或已经进入放电状态，持续恢复时间后清除。 */
    if (bms_fault_afe_recovery_ready(data->afe_chg_oc1,
                                     afe_status_ready &&
                                     ((data->current_10ma <= AFE_CLEAR_OCC_RECOVER_CUR_10MA) || data->discharging),
                                     APP_HW_AFE_CLEAR_OCC1,
                                     clear_wait_mask,
                                     &dly_occ1_recover,
                                     AFE_CLEAR_OCC_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_OCC1;
    }

    if (bms_fault_afe_recovery_ready(data->afe_chg_oc2,
                                     afe_status_ready &&
                                     ((data->current_10ma <= AFE_CLEAR_OCC_RECOVER_CUR_10MA) || data->discharging),
                                     APP_HW_AFE_CLEAR_OCC2,
                                     clear_wait_mask,
                                     &dly_occ2_recover,
                                     AFE_CLEAR_OCC_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_OCC2;
    }

    /* 检测到有效充电电流时，AFE放电过流硬件标志立即发送清除命令。 */
    if (afe_status_ready && data->charging)
    {
        if (data->afe_dsg_oc1 && ((clear_wait_mask & APP_HW_AFE_CLEAR_OCD1) == 0U))
        {
            clear_mask |= APP_HW_AFE_CLEAR_OCD1;
        }
        if (data->afe_dsg_oc2 && ((clear_wait_mask & APP_HW_AFE_CLEAR_OCD2) == 0U))
        {
            clear_mask |= APP_HW_AFE_CLEAR_OCD2;
        }

        (void)d_timer_delay_check(false, &dly_ocd1_recover, AFE_CLEAR_OCD_RECOVER_TIME);
        (void)d_timer_delay_check(false, &dly_ocd2_recover, AFE_CLEAR_OCD_RECOVER_TIME);
    }
    else
    {
        /* 无充电电流时，仍按恢复电流和30秒恢复时间处理。 */
        if (bms_fault_afe_recovery_ready(data->afe_dsg_oc1,
                                          afe_status_ready && (data->current_10ma >= AFE_CLEAR_OCD_RECOVER_CUR_10MA),
                                         APP_HW_AFE_CLEAR_OCD1,
                                         clear_wait_mask,
                                         &dly_ocd1_recover,
                                         AFE_CLEAR_OCD_RECOVER_TIME))
        {
            clear_mask |= APP_HW_AFE_CLEAR_OCD1;
        }

        if (bms_fault_afe_recovery_ready(data->afe_dsg_oc2,
                                          afe_status_ready && (data->current_10ma >= AFE_CLEAR_OCD_RECOVER_CUR_10MA),
                                         APP_HW_AFE_CLEAR_OCD2,
                                         clear_wait_mask,
                                         &dly_ocd2_recover,
                                         AFE_CLEAR_OCD_RECOVER_TIME))
        {
            clear_mask |= APP_HW_AFE_CLEAR_OCD2;
        }
    }

    if (bms_fault_afe_recovery_ready(data->afe_short_circuit,
                                     afe_status_ready && (data->current_10ma >= AFE_CLEAR_OCD_RECOVER_CUR_10MA),
                                     APP_HW_AFE_CLEAR_SCD,
                                     clear_wait_mask,
                                     &dly_scd_recover,
                                     AFE_CLEAR_SCD_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_SCD;
    }

    /* IWF只有在状态寄存器和AFE_NTC读取均恢复正常后才允许清除。 */
    if (bms_fault_afe_recovery_ready(data->afe_iic_watchdog,
                                     afe_status_ready && !data->afe_ntc_iic_fault,
                                     APP_HW_AFE_CLEAR_IWF,
                                     clear_wait_mask,
                                     &dly_iwf_recover,
                                     AFE_CLEAR_IIC_RECOVER_TIME))
    {
        clear_mask |= APP_HW_AFE_CLEAR_IWF;
    }

    if (clear_mask != APP_HW_AFE_CLEAR_NONE)
    {
        /* 成功后记录等待状态，不修改本地AFE故障缓存；故障位持续存在时由重试计时器再次发送。 */
        if (app_hw_apply_clear_afe_fault(clear_mask) != 0U)
        {
            clear_wait_mask |= clear_mask;
            (void)d_timer_delay_check(false,
                                      &dly_afe_clear_retry,
                                      BMS_AFE_CLEAR_RETRY_TIME);
        }
    }

    s_bms_fault_last_clear_mask = clear_mask;
    s_bms_fault_clear_wait_mask = clear_wait_mask;
}

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U)
static void bms_fault_update_voltage_faults(const bms_fault_data_t *data)
{
#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE != 0U)
    static app_bms_cuv_temp_band_state_t cuv1_temp_band_state;
    app_bms_cuv_temp_band_t cuv1_temp_band;
    uint16_t cuv1_set_pro_voltage_mv;
    uint16_t cuv1_recover_pro_voltage_mv;
#endif
#if (BMS_PROTECT_CELL_OV_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cov_war);
#endif
#if (BMS_PROTECT_CELL_OV_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cov_pro);
#endif
#if (BMS_PROTECT_CELL_OV2_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cov2_pro);
#endif
#if (BMS_PROTECT_CELL_UV_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cuv_war);
#endif
#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cuv1_pro);
#endif
#if (BMS_PROTECT_CELL_UV2_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cuv2_pro);
#endif
#if (BMS_PROTECT_CELL_DEEP_UV_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_dcuv_pro_set);
#endif
#if (BMS_PROTECT_PACK_OV_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_pov_war);
#endif
#if (BMS_PROTECT_PACK_OV_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_pov_pro);
#endif
#if (BMS_PROTECT_PACK_UV_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_puv_war);
#endif
#if (BMS_PROTECT_PACK_UV_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_puv_pro);
#endif
#if (BMS_PROTECT_PACK_OV_WARN_ENABLE != 0U)
    uint32_t pack_ov_war_mv;
#endif
#if (BMS_PROTECT_PACK_OV_PRO_ENABLE != 0U)
    uint32_t pack_ov_pro_mv;
#endif
#if (BMS_PROTECT_PACK_UV_WARN_ENABLE != 0U)
    uint32_t pack_uv_war_mv;
#endif
#if (BMS_PROTECT_PACK_UV_PRO_ENABLE != 0U)
    uint32_t pack_uv_pro_mv;
#endif

#if (BMS_PROTECT_PACK_OV_WARN_ENABLE != 0U)
    pack_ov_war_mv = bms_pack_threshold(COV_SET_WAR_VOLTAGE, data->cell_count);
#endif
#if (BMS_PROTECT_PACK_OV_PRO_ENABLE != 0U)
    pack_ov_pro_mv = bms_pack_threshold(COV_SET_PRO_VOLTAGE, data->cell_count);
#endif
#if (BMS_PROTECT_PACK_UV_WARN_ENABLE != 0U)
    pack_uv_war_mv = bms_pack_threshold(CUV_SET_WAR_VOLTAGE, data->cell_count);
#endif
#if (BMS_PROTECT_PACK_UV_PRO_ENABLE != 0U)
    pack_uv_pro_mv = bms_pack_threshold(CUV2_SET_PRO_VOLTAGE, data->cell_count);
#endif

#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE != 0U)
    cuv1_temp_band = app_bms_cuv_temp_band_update(
        &cuv1_temp_band_state,
        data->min_cell_temp_c,
        (uint8_t)data->temp_data_ready,
        CUV1_TEMP_WARM_BOUNDARY_C,
        CUV1_TEMP_COLD_BOUNDARY_C,
        CUV1_TEMP_BAND_HYSTERESIS_C);
    switch (cuv1_temp_band)
    {
        case APP_BMS_CUV_TEMP_BAND_MID:
            cuv1_set_pro_voltage_mv = CUV1_SET_PRO_MID_VOLTAGE_MV;
            cuv1_recover_pro_voltage_mv = CUV1_RECOVER_PRO_MID_VOLTAGE_MV;
            break;

        case APP_BMS_CUV_TEMP_BAND_COLD:
            cuv1_set_pro_voltage_mv = CUV1_SET_PRO_COLD_VOLTAGE_MV;
            cuv1_recover_pro_voltage_mv = CUV1_RECOVER_PRO_COLD_VOLTAGE_MV;
            break;

        case APP_BMS_CUV_TEMP_BAND_WARM:
        default:
            cuv1_set_pro_voltage_mv = CUV1_SET_PRO_WARM_VOLTAGE_MV;
            cuv1_recover_pro_voltage_mv = CUV1_RECOVER_PRO_WARM_VOLTAGE_MV;
            break;
    }
#endif

#if (BMS_PROTECT_CELL_OV_WARN_ENABLE != 0U)
    /* cell_war_cov 置位：最高单体电压达到或超过一级过压告警阈值。 */
    BMS_DELAY_SET(data->max_cell_mv >= COV_SET_WAR_VOLTAGE,
                  dly_cov_war_set,
                  COV_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov);
    /* cell_war_cov 清除：最高单体不高于恢复阈值，或系统进入放电状态。 */
    BMS_DELAY_CLEAR((data->max_cell_mv <= COV_RECOVER_WAR_VOLTAGE) || data->discharging,
                    dly_cov_war_recover,
                    COV_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov);
#endif

#if (BMS_PROTECT_CELL_OV_PRO_ENABLE != 0U)
    /* cell_pro_cov 置位：最高单体电压达到或超过一级过压保护阈值。 */
    BMS_DELAY_SET(data->max_cell_mv >= COV_SET_PRO_VOLTAGE,
                  dly_cov_pro_set,
                  COV_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov);
    /* cell_pro_cov 清除：最高单体不高于保护恢复阈值，或系统进入放电状态。 */
    BMS_DELAY_CLEAR((data->max_cell_mv <= COV_RECOVER_PRO_VOLTAGE) || data->discharging,
                    dly_cov_pro_recover,
                    COV_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov);
#endif

#if (BMS_PROTECT_CELL_OV2_PRO_ENABLE != 0U)
    /* cell_pro_cov2 置位：最高单体电压达到或超过二级过压保护阈值。 */
    BMS_DELAY_SET(data->max_cell_mv >= COV2_SET_PRO_VOLTAGE,
                  dly_cov2_pro_set,
                  COV2_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2);
    /* cell_pro_cov2 清除：最高单体不高于二级过压恢复阈值。 */
    BMS_DELAY_CLEAR(data->max_cell_mv <= COV2_RECOVER_PRO_VOLTAGE,
                    dly_cov2_pro_recover,
                    COV2_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2);
#endif

#if (BMS_PROTECT_PACK_OV_WARN_ENABLE != 0U)
    /* pack_war_pov 置位：总压达到或超过单体一级过压告警阈值乘串数。 */
    BMS_DELAY_SET(data->pack_voltage_mv >= pack_ov_war_mv,
                  dly_pov_war_set,
                  COV_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_war_pov);
    /* pack_war_pov 清除：总压不高于单体一级过压告警恢复阈值乘串数。 */
    BMS_DELAY_CLEAR(data->pack_voltage_mv <= bms_pack_threshold(COV_RECOVER_WAR_VOLTAGE, data->cell_count),
                    dly_pov_war_recover,
                    COV_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_war_pov);
#endif

#if (BMS_PROTECT_PACK_OV_PRO_ENABLE != 0U)
    /* pack_pro_pov 置位：总压达到或超过单体一级过压保护阈值乘串数。 */
    BMS_DELAY_SET(data->pack_voltage_mv >= pack_ov_pro_mv,
                  dly_pov_pro_set,
                  COV_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov);
    /* pack_pro_pov 清除：总压不高于保护恢复阈值乘串数，或系统进入放电状态。 */
    BMS_DELAY_CLEAR((data->pack_voltage_mv <= bms_pack_threshold(COV_RECOVER_PRO_VOLTAGE, data->cell_count)) || data->discharging,
                    dly_pov_pro_recover,
                    COV_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov);
#endif

#if (BMS_PROTECT_CELL_UV_WARN_ENABLE != 0U)
    /* cell_war_cuv 置位：最低单体电压达到或低于一级欠压告警阈值。 */
    BMS_DELAY_SET(data->min_cell_mv <= CUV_SET_WAR_VOLTAGE,
                  dly_cuv_war_set,
                  CUV_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv);
    /* cell_war_cuv 清除：最低单体不低于告警恢复阈值，或系统进入充电状态。 */
    BMS_DELAY_CLEAR((data->min_cell_mv >= CUV_RECOVER_WAR_VOLTAGE) || data->charging,
                    dly_cuv_war_recover,
                    CUV_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv);
#endif

#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE != 0U)
    /* CUV1：最低单体达到或低于当前温度档的一级欠压阈值后置位。 */
    BMS_DELAY_SET(data->min_cell_mv <= cuv1_set_pro_voltage_mv,
                  dly_cuv1_pro_set,
                  CUV1_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1);
    /* CUV1：不低于当前温度档的一级恢复阈值，或进入充电状态。 */
    BMS_DELAY_CLEAR((data->min_cell_mv >= cuv1_recover_pro_voltage_mv) || data->charging,
                    dly_cuv1_pro_recover,
                    CUV1_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1);
#endif

#if (BMS_PROTECT_CELL_UV2_PRO_ENABLE != 0U)
    /* CUV2：独立于CUV1置位二级欠压故障。 */
    BMS_DELAY_SET(data->min_cell_mv <= CUV2_SET_PRO_VOLTAGE,
                  dly_cuv2_pro_set,
                  CUV2_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2);
    /* CUV2：只清除二级故障，不影响一级故障。 */
    BMS_DELAY_CLEAR((data->min_cell_mv >= CUV2_RECOVER_PRO_VOLTAGE) || data->charging,
                    dly_cuv2_pro_recover,
                    CUV2_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2);
#endif

#if (BMS_PROTECT_CELL_DEEP_UV_ENABLE != 0U)
    /* cell_deep_cuv 置位：最低单体达到或低于深度欠压阈值；本函数内不做自动清除。 */
    BMS_DELAY_SET(data->min_cell_mv <= DCUV_SET_PRO_VOLTAGE,
                  dly_dcuv_pro_set,
                  DCUV_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv);
#endif

#if (BMS_PROTECT_PACK_UV_WARN_ENABLE != 0U)
    /* pack_war_cuv 置位：总压达到或低于单体一级欠压告警阈值乘串数。 */
    BMS_DELAY_SET(data->pack_voltage_mv <= pack_uv_war_mv,
                  dly_puv_war_set,
                  CUV_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv);
    /* pack_war_cuv 清除：总压不低于告警恢复阈值乘串数，或系统进入充电状态。 */
    BMS_DELAY_CLEAR((data->pack_voltage_mv >= bms_pack_threshold(CUV_RECOVER_WAR_VOLTAGE, data->cell_count)) || data->charging,
                    dly_puv_war_recover,
                    CUV_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv);
#endif

#if (BMS_PROTECT_PACK_UV_PRO_ENABLE != 0U)
    /* pack_pro_cuv 置位：总压达到或低于单体二级欠压保护阈值乘串数。 */
    BMS_DELAY_SET(data->pack_voltage_mv <= pack_uv_pro_mv,
                  dly_puv_pro_set,
                  CUV2_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv);
    /* pack_pro_cuv 清除：总压不低于二级欠压恢复阈值乘串数，或系统进入充电状态。 */
    BMS_DELAY_CLEAR((data->pack_voltage_mv >= bms_pack_threshold(CUV2_RECOVER_PRO_VOLTAGE, data->cell_count)) || data->charging,
                    dly_puv_pro_recover,
                    CUV2_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv);
#endif
}
#endif

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U)
static void bms_fault_update_temp_faults(const bms_fault_data_t *data)
{
#if (BMS_PROTECT_CHG_OT_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_otc_war);
#endif
#if (BMS_PROTECT_CHG_OT_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_otc_pro);
#endif
#if (BMS_PROTECT_CHG_UT_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_utc_war);
#endif
#if (BMS_PROTECT_CHG_UT_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_utc_pro);
#endif
#if (BMS_PROTECT_DSG_OT_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_otd_war);
#endif
#if (BMS_PROTECT_DSG_OT_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_otd_pro);
#endif
#if (BMS_PROTECT_DSG_UT_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_utd_war);
#endif
#if (BMS_PROTECT_DSG_UT_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_utd_pro);
#endif
#if (BMS_PROTECT_MOS_OT_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_mos_ot_war);
#endif
#if (BMS_PROTECT_MOS_OT_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_mos_ot_pro);
#endif

#if (BMS_PROTECT_CHG_OT_WARN_ENABLE != 0U)
    /* pack_war_otc 置位：BMS处于充电状态且温度达到充电高温告警阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                  (data->max_cell_temp_c >= OTC_SET_WAR_TEMP),
                  dly_otc_war_set,
                  OTC_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc);
    /* pack_war_otc 清除：温度回落到充电高温告警恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->max_cell_temp_c <= OTC_RECOVER_WAR_TEMP),
                    dly_otc_war_recover,
                    OTC_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc);
#endif

#if (BMS_PROTECT_CHG_OT_PRO_ENABLE != 0U)
    /* pack_pro_otc 置位：BMS处于充电状态且温度达到充电高温保护阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                  (data->max_cell_temp_c >= OTC_SET_PRO_TEMP),
                  dly_otc_pro_set,
                  OTC_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc);
    /* pack_pro_otc 清除：温度回落到充电高温保护恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->max_cell_temp_c <= OTC_RECOVER_PRO_TEMP),
                    dly_otc_pro_recover,
                    OTC_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc);
#endif

#if (BMS_PROTECT_CHG_UT_WARN_ENABLE != 0U)
    /* pack_war_utc 置位：充电状态下温度达到或低于充电低温告警阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                  (data->min_cell_temp_c <= UTC_SET_WAR_TEMP),
                  dly_utc_war_set,
                  UTC_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_utc);
    /* pack_war_utc 清除：温度恢复到充电低温告警恢复阈值以上。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->min_cell_temp_c >= UTC_RECOVER_WAR_TEMP),
                    dly_utc_war_recover,
                    UTC_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_utc);
#endif

#if (BMS_PROTECT_CHG_UT_PRO_ENABLE != 0U)
    /* pack_pro_utc 置位：充电状态下温度达到或低于充电低温保护阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                  (data->min_cell_temp_c <= UTC_SET_PRO_TEMP),
                  dly_utc_pro_set,
                  UTC_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc);
    /* pack_pro_utc 清除：温度恢复到充电低温保护恢复阈值以上。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->min_cell_temp_c >= UTC_RECOVER_PRO_TEMP),
                    dly_utc_pro_recover,
                    UTC_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc);
#endif

#if (BMS_PROTECT_DSG_OT_WARN_ENABLE != 0U)
    /* pack_war_otd 置位：非充电状态下温度达到放电高温告警阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status != APP_BMS_STATUS_CHARGE) &&
                  (data->max_cell_temp_c >= OTD_SET_WAR_TEMP),
                  dly_otd_war_set,
                  OTD_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd);
    /* pack_war_otd 清除：温度回落到放电高温告警恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->max_cell_temp_c <= OTD_RECOVER_WAR_TEMP),
                    dly_otd_war_recover,
                    OTD_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd);
#endif

#if (BMS_PROTECT_DSG_OT_PRO_ENABLE != 0U)
    /* pack_pro_otd 置位：非充电状态下温度达到放电高温保护阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status != APP_BMS_STATUS_CHARGE) &&
                  (data->max_cell_temp_c >= OTD_SET_PRO_TEMP),
                  dly_otd_pro_set,
                  OTD_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd);
    /* pack_pro_otd 清除：温度回落到放电高温保护恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->max_cell_temp_c <= OTD_RECOVER_PRO_TEMP),
                    dly_otd_pro_recover,
                    OTD_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd);
#endif

#if (BMS_PROTECT_DSG_UT_WARN_ENABLE != 0U)
    /* pack_war_utd 置位：非充电状态下温度达到或低于放电低温告警阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status != APP_BMS_STATUS_CHARGE) &&
                  (data->min_cell_temp_c <= UTD_SET_WAR_TEMP),
                  dly_utd_war_set,
                  UTD_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_utd);
    /* pack_war_utd 清除：温度恢复到放电低温告警恢复阈值以上。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->min_cell_temp_c >= UTD_RECOVER_WAR_TEMP),
                    dly_utd_war_recover,
                    UTD_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_utd);
#endif

#if (BMS_PROTECT_DSG_UT_PRO_ENABLE != 0U)
    /* pack_pro_utd 置位：非充电状态下温度达到或低于放电低温保护阈值。 */
    BMS_DELAY_SET(data->temp_data_ready &&
                  (data->bms_work_status != APP_BMS_STATUS_CHARGE) &&
                  (data->min_cell_temp_c <= UTD_SET_PRO_TEMP),
                  dly_utd_pro_set,
                  UTD_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd);
    /* pack_pro_utd 清除：温度恢复到放电低温保护恢复阈值以上。 */
    BMS_DELAY_CLEAR(data->temp_data_ready &&
                    (data->min_cell_temp_c >= UTD_RECOVER_PRO_TEMP),
                    dly_utd_pro_recover,
                    UTD_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd);
#endif

#if (BMS_PROTECT_MOS_OT_WARN_ENABLE != 0U)
    /* MOS过温告警置位：有效MOS_NTC温度达到或超过告警阈值，连续满足配置时间。 */
    BMS_DELAY_SET(data->mos_temp_valid &&
                  (data->mos_temp_0p1c >= MOS_OT_SET_WAR_TEMP_0P1C),
                  dly_mos_ot_war_set,
                  MOS_OT_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_waring);
    /* MOS过温告警清除：有效MOS_NTC温度不高于恢复阈值，连续满足配置时间。 */
    BMS_DELAY_CLEAR(data->mos_temp_valid &&
                    (data->mos_temp_0p1c <= MOS_OT_RECOVER_TEMP_0P1C),
                    dly_mos_ot_war_recover,
                    MOS_OT_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_waring);
#endif

#if (BMS_PROTECT_MOS_OT_PRO_ENABLE != 0U)
    /* MOS过温保护置位：有效MOS_NTC温度达到或超过保护阈值，连续满足配置时间。 */
    BMS_DELAY_SET(data->mos_temp_valid &&
                  (data->mos_temp_0p1c >= MOS_OT_SET_PRO_TEMP_0P1C),
                  dly_mos_ot_pro_set,
                  MOS_OT_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP);
    /* MOS过温保护清除：有效MOS_NTC温度不高于恢复阈值，连续满足配置时间。 */
    BMS_DELAY_CLEAR(data->mos_temp_valid &&
                    (data->mos_temp_0p1c <= MOS_OT_RECOVER_PRO_TEMP_0P1C),
                    dly_mos_ot_pro_recover,
                    MOS_OT_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP);
#endif
}
#endif

static bool bms_fault_dsg_oc_short_active(void)
{
    app_bms_snapshot_t snapshot;
    const t_bms_fault_flag *sw_fault = &gt_app_bms_fault.tbms_sw_fault_flag;

    app_bms_snapshot_read(&snapshot);

    /* LD只用于放电过流保护/锁定及短路恢复，告警按电流和时间独立恢复。 */
    if ((sw_fault->udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U) ||
        (sw_fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 != 0U) ||
        (sw_fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 != 0U) ||
        (sw_fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 != 0U) ||
        (sw_fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 != 0U) ||
        (sw_fault->udischarge_fault_B.tdischarge_fault_B.b1pack_shrot != 0U) ||
        (snapshot.afe_fault.dsg_oc1 != 0U) ||
        (snapshot.afe_fault.dsg_oc2 != 0U) ||
        (snapshot.afe_fault.short_circuit != 0U))
    {
        return true;
    }

    return false;
}

static void bms_fault_clear_dsg_oc_short(void)
{
    /* 负载已经移除时，只提前恢复放电过流保护/锁定和短路；告警仍按电流与延时恢复。 */
    bms_fault_rtt_ocd_reset(
        "LD",
        s_bms_ocd_recover_count,
        (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock);
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock = 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 = 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 = 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 = 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 = 0U;
    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot = 0U;

    s_bms_ocd_recover_count = 0U;

    /* 清AFE放电过流/短路硬件标志；DataHandle_Task将在下一个10ms周期刷新快照。 */
    app_hw_apply_clear_afe_dsg_oc_short_fault();
}

static void bms_fault_update_load_removed_recovery(const bms_fault_data_t *data)
{
#if (BMS_LOAD_DETECT_ENABLE != 0U)
    static uint8_t detect_enabled;
    (void)data;

    if (bms_fault_dsg_oc_short_active() == false)
    {
        /* 没有放电过流/短路时关闭检测电路，避免这一路额外消耗静态电流。 */
        detect_enabled = 0U;
        app_hw_apply_load_detect_enable((uint8_t)(BMS_LOAD_DET_EN_ACTIVE_LEVEL ? 0U : 1U));
        return;
    }

    /* 有放电过流/短路故障时使能LD；短路可能在采到放电电流前就切断MOS。 */
    app_hw_apply_load_detect_enable(BMS_LOAD_DET_EN_ACTIVE_LEVEL);
    if (detect_enabled == 0U)
    {
        /* 使能后丢弃关闭期间的旧状态；输入模块从“负载在位”开始重新消抖。 */
        app_input_signals_load_detect_rearm();
        detect_enabled = 1U;
        return;
    }

    /* 仅LD_EN开启后的消抖结果有效；检测电路关闭时适配层返回无效值。 */
    if (app_hw_apply_load_detect_level() == BMS_LOAD_REMOVED_LEVEL)
    {
        bms_fault_clear_dsg_oc_short();
        detect_enabled = 0U;
        app_hw_apply_load_detect_enable((uint8_t)(BMS_LOAD_DET_EN_ACTIVE_LEVEL ? 0U : 1U));
    }
#else
    app_hw_apply_load_detect_enable((uint8_t)(BMS_LOAD_DET_EN_ACTIVE_LEVEL ? 0U : 1U));
#endif
}

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U)
static void bms_fault_update_current_faults(const bms_fault_data_t *data)
{
#if (BMS_PROTECT_CHG_OC_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_occ_war);
#endif
#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_occ1_pro);
    BMS_DECLARE_DELAY_PAIR(dly_occ2_pro);
#endif
#if (BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_recharge_occ_pro);
#endif
#if (BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_recharge_occ_war);
#endif
#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd1_war_set);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd2_war_set);
#endif
#if (BMS_PROTECT_DSG_OC_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd1_pro_set);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd2_pro_set);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd3_pro_set);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd4_pro_set);
#endif
#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd_war_recover);
#endif
#if (BMS_PROTECT_DSG_OC_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd1_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd2_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd3_recover);
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd4_recover);
#endif
#if (BMS_PROTECT_SHORT_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_short);
#endif
#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    static uint16_t occ_recover_count;
#endif
#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
    uint8_t ocd_warning_before;
#endif

#if (BMS_PROTECT_CHG_OC_WARN_ENABLE != 0U)
    /* 普通充电过流告警只在BMS处于充电状态时置位。 */
    BMS_DELAY_SET((data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                  (data->current_10ma >= OCC_WAR_SET_CUR_10MA),
                  dly_occ_war_set,
                  OCC_WAR_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ);
    /* 充电过流告警清除：电流回落到告警恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->current_10ma <= OCC_WAR_RECOVER_CUR_10MA,
                    dly_occ_war_recover,
                    OCC_WAR_RECOVER_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ);
#endif

#if (BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE != 0U)
    /* 回馈过流告警：沿用保护的接线识别条件，按配置阈值和延时告警。 */
    BMS_DELAY_SET(data->detd_present &&
                      (data->detc_present == false) &&
                      (data->current_10ma >= RECHARGE_OCC_SET_WAR_CUR_10MA),
                  dly_recharge_occ_war_set,
                  RECHARGE_OCC_SET_WAR_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc);

    /* 回馈电流降至回馈告警恢复阈值并持续配置时间后清除。 */
    BMS_DELAY_CLEAR(data->current_10ma <= RECHARGE_OCC_RECOVER_WAR_CUR_10MA,
                    dly_recharge_occ_war_recover,
                    RECHARGE_OCC_RECOVER_WAR_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc);
#endif

#if (BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE != 0U)
    /* 回馈过流：仅在DETD在位、DETC不在位时，正向充电电流达到或超过阈值并持续后保护。 */
    BMS_DELAY_SET(data->detd_present &&
                      (data->detc_present == false) &&
                  (data->current_10ma >= RECHARGE_OCC_SET_PRO_CUR_10MA),
                  dly_recharge_occ_pro_set,
                  RECHARGE_OCC_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc);

    /* 回馈电流恢复到近零并持续配置时间后清除。 */
    BMS_DELAY_CLEAR(data->current_10ma <= RECHARGE_OCC_RECOVER_PRO_CUR_10MA,
                    dly_recharge_occ_pro_recover,
                    RECHARGE_OCC_RECOVER_PRO_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc);
#endif

#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    /* OCC1独立置位：BMS处于充电状态且一级充电过流持续到保护时间。 */
    if (d_timer_delay_check((data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                            (data->current_10ma >= OCC1_SET_PRO_CUR_10MA),
                  &dly_occ1_pro_set,
                  OCC1_SET_PRO_TIME) == Ok)
    {
        /* 同一次持续过流只计数一次，不能因计时器周期到期反复累计。 */
        if (gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 == 0U)
        {
            occ_recover_count++;
        }
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 = 1U;
    }

    /* OCC2独立置位：BMS处于充电状态且二级充电过流持续到保护时间。 */
    if (d_timer_delay_check((data->bms_work_status == APP_BMS_STATUS_CHARGE) &&
                            (data->current_10ma >= OCC2_SET_PRO_CUR_10MA),
                  &dly_occ2_pro_set,
                  OCC2_SET_PRO_TIME) == Ok)
    {
        /* 同一次持续二级过流只计数一次，不能因500ms计时周期重复累计。 */
        if (gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 == 0U)
        {
            occ_recover_count++;
        }
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 = 1U;
    }

    /* 充电过流保护恢复：放电电流出现时立即清除两个等级和锁定，否则按各自计时器恢复。 */
    if (data->discharging)
    {
        occ_recover_count = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock = 0U;
        (void)d_timer_delay_check(false, &dly_occ1_pro_recover, OCC1_RECOVER_TIME);
        (void)d_timer_delay_check(false, &dly_occ2_pro_recover, OCC1_RECOVER_TIME);
    }
    else
    {
        BMS_DELAY_CLEAR(data->current_10ma <= OCC1_RECOVER_CUR_10MA,
                        dly_occ1_pro_recover,
                        OCC1_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1);
        BMS_DELAY_CLEAR(data->current_10ma <= OCC1_RECOVER_CUR_10MA,
                        dly_occ2_pro_recover,
                        OCC1_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2);
    }

    /* 离开充电状态表示本次充电会话结束：清锁定和累计次数；OCC1/OCC2仍按原恢复条件清除。 */
    if (data->bms_work_status != APP_BMS_STATUS_CHARGE)
    {
        occ_recover_count = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock = 0U;
    }
#endif

    /* AFE硬件过流只参与最终OCC保护标志，不能单独触发软件充电过流锁。 */
#if (BMS_PROTECT_CHG_OC_LOCK_ENABLE != 0U) && (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    if ((occ_recover_count >= OCC_LOCK_COUNT_LIMIT) &&
        ((gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 != 0U) ||
         (gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 != 0U)))
    {
        /* pack_occ_lock置位：软件充电过流保护事件累计达到配置次数后锁定。 */
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock = 1U;
    }
#else
    /* 软件充电过流保护关闭时，禁止累计次数参与充电过流锁定。 */
#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    occ_recover_count = 0U;
#endif
#endif

#if (BMS_PROTECT_CHG_OC_LOCK_ENABLE != 0U)
    if ((gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock != 0U) &&
        data->discharging)
    {
        /* pack_occ_lock 清除：锁定后检测到放电电流，认为负载释放并清零累计次数。 */
        occ_recover_count = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock = 0U;
    }
#endif

#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
    /* 放电过流告警统一为一个标志；保留两个等级的阈值和延时，任一级达到条件即置位。 */
    ocd_warning_before = (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd;
    BMS_DELAY_SET(data->current_10ma <= OCD1_WAR_SET_CUR_10MA,
                  dly_ocd1_war_set,
                  OCD1_WAR_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd);
    BMS_DELAY_SET(data->current_10ma <= OCD2_WAR_SET_CUR_10MA,
                  dly_ocd2_war_set,
                  OCD2_WAR_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd);
    BMS_DELAY_CLEAR((data->current_10ma >= OCD1_WAR_RECOVER_CUR_10MA) &&
                    (data->current_10ma >= OCD2_WAR_RECOVER_CUR_10MA),
                    dly_ocd_war_recover,
                    OCD1_WAR_RECOVER_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd);
    if (ocd_warning_before !=
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd)
    {
        bms_fault_rtt_ocd_warning((ocd_warning_before == 0U) ? "SET" : "CLEAR_I");
    }
#endif

#if (BMS_PROTECT_DSG_OC_PRO_ENABLE != 0U)
    /* OCD1~OCD4分别置位，任意一级都可以独立关联到上位机和保护汇总。 */
    if (d_timer_delay_check(data->current_10ma <= OCD1_SET_PRO_CUR_10MA,
                            &dly_ocd1_pro_set,
                            OCD1_SET_PRO_TIME) == Ok)
    {
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 = 1U;
        s_bms_ocd_recover_count++;
        bms_fault_rtt_ocd_count("SW1", data->current_10ma);
    }
    if (d_timer_delay_check(data->current_10ma <= OCD2_SET_PRO_CUR_10MA,
                            &dly_ocd2_pro_set,
                            OCD2_SET_PRO_TIME) == Ok)
    {
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 = 1U;
        s_bms_ocd_recover_count++;
        bms_fault_rtt_ocd_count("SW2", data->current_10ma);
    }
    if (d_timer_delay_check(data->current_10ma <= OCD3_SET_PRO_CUR_10MA,
                            &dly_ocd3_pro_set,
                            OCD3_SET_PRO_TIME) == Ok)
    {
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 = 1U;
        s_bms_ocd_recover_count++;
        bms_fault_rtt_ocd_count("SW3", data->current_10ma);
    }
    if (d_timer_delay_check(data->current_10ma <= OCD4_SET_PRO_CUR_10MA,
                            &dly_ocd4_pro_set,
                            OCD4_SET_PRO_TIME) == Ok)
    {
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 = 1U;
        s_bms_ocd_recover_count++;
        bms_fault_rtt_ocd_count("SW4", data->current_10ma);
    }

    /* 检测到有效充电电流时，OCD1~OCD4软件保护立即恢复。 */
    if (data->charging)
    {
        if (gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 != 0U)
        {
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 = 0U;
        }
        if (gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 != 0U)
        {
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 = 0U;
        }
        if (gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 != 0U)
        {
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 = 0U;
        }
        if (gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 != 0U)
        {
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 = 0U;
        }

        (void)d_timer_delay_check(false, &dly_ocd1_recover, OCD_RECOVER_TIME);
        (void)d_timer_delay_check(false, &dly_ocd2_recover, OCD_RECOVER_TIME);
        (void)d_timer_delay_check(false, &dly_ocd3_recover, OCD_RECOVER_TIME);
        (void)d_timer_delay_check(false, &dly_ocd4_recover, OCD_RECOVER_TIME);
    }
    else
    {
        /* 无充电电流时，保持原有放电电流恢复阈值和30秒恢复时间。 */
        BMS_DELAY_CLEAR(data->current_10ma >= OCD_RECOVER_CUR_10MA,
                        dly_ocd1_recover,
                        OCD_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1);
        BMS_DELAY_CLEAR(data->current_10ma >= OCD_RECOVER_CUR_10MA,
                        dly_ocd2_recover,
                        OCD_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2);
        BMS_DELAY_CLEAR(data->current_10ma >= OCD_RECOVER_CUR_10MA,
                        dly_ocd3_recover,
                        OCD_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3);
        BMS_DELAY_CLEAR(data->current_10ma >= OCD_RECOVER_CUR_10MA,
                        dly_ocd4_recover,
                        OCD_RECOVER_TIME,
                        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4);
    }

#endif

#if (BMS_PROTECT_SHORT_ENABLE != 0U)
    /* pack_shrot 置位：软件短路电流阈值触发；AFE短路只在最终故障合并时参与。 */
    /* PDS阈值按负数保存，必须直接比较，避免0A被误判为短路。 */
    BMS_DELAY_SET((data->current_10ma <= PDS_SET_PRO_CUR_10MA),
                  dly_short_set,
                  PDS_SET_PRO_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot);
    /* pack_shrot 清除：电流恢复到放电过流恢复阈值以内并持续恢复时间。 */
    BMS_DELAY_CLEAR(data->current_10ma >= OCD_RECOVER_CUR_10MA,
                    dly_short_recover,
                    PDS_RECOVER_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot);
#endif

#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
    s_bms_occ_recover_count = occ_recover_count;
#else
    s_bms_occ_recover_count = 0U;
#endif

}
#endif

static void bms_fault_update_dsg_oc_short_lock(const bms_fault_data_t *data)
{
#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U) && (BMS_PROTECT_DSG_OC_LOCK_ENABLE != 0U)
    const uint8_t software_short = (uint8_t)
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot;

    /* AFE状态无效时保留原边沿基线，避免短暂读取失败后将同一次短路重复计数。 */
    if ((data->afe_status_valid != false) || (software_short != 0U))
    {
        const uint8_t short_active = (uint8_t)((software_short != 0U) ||
            ((data->afe_status_valid != false) && (data->afe_short_circuit != false)));

        if ((short_active != 0U) && (s_bms_short_event_previous_active == 0U))
        {
            if (s_bms_ocd_recover_count < 0xFFFFU)
            {
                s_bms_ocd_recover_count++;
            }
            bms_fault_rtt_ocd_count("SHORT", data->current_10ma);
        }
        s_bms_short_event_previous_active = short_active;
    }

    if (s_bms_ocd_recover_count >= OCD_LOCK_COUNT_LIMIT)
    {
        const uint8_t was_locked = (uint8_t)
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock;

        /* 放电过流和短路共用锁计数及锁定位；锁定后持续禁止放电。 */
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock = 1U;
        if (was_locked == 0U)
        {
            bms_fault_rtt_ocd_count("LOCK", data->current_10ma);
        }
    }

    if ((gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U) &&
        data->data_ready && data->charging)
    {
        /* 与原过流锁一致：检测到充电电流后清锁并清零共用累计次数。 */
        bms_fault_rtt_ocd_reset(
            "charge",
            s_bms_ocd_recover_count,
            (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock);
        s_bms_ocd_recover_count = 0U;
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock = 0U;
    }
#else
    (void)data;
#endif
}

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U)
static void bms_fault_update_general_faults(const bms_fault_data_t *data)
{
#if (BMS_PROTECT_CELL_DIFF_WARN_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cell_diff_war);
#endif
#if (BMS_PROTECT_CELL_DIFF_PRO_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_cell_diff_pro);
#endif
#if (BMS_PROTECT_CELL_DIFF_PRO2_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_cell_diff_pro2_set);
#endif
#if (BMS_PROTECT_CHG_MOS_FAIL_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_chg_mos_fail_set);
#endif
#if (BMS_PROTECT_DSG_MOS_FAIL_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_dsg_mos_fail_set);
#endif
#if (BMS_PROTECT_NTC_FAULT_ENABLE != 0U)
    BMS_DECLARE_DELAY_PAIR(dly_ntc_fault);
#endif

#if (BMS_PROTECT_NTC_FAULT_ENABLE != 0U)
    /* NTC-Fault保护置位：至少两路NTC电气异常，且电池处于空闲状态。 */
    BMS_DELAY_SET(data->ntc_fault && data->ntc_idle,
                  dly_ntc_fault_set,
                  NTC_FAULT_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault);
    /* NTC-Fault保护清除：异常数量不高于触发数减一并持续恢复时间；与当前电流无关。 */
    BMS_DELAY_CLEAR(data->ntc_abnormal_count <= (BMS_NTC_FAULT_MIN_COUNT - 1U),
                    dly_ntc_fault_recover,
                    NTC_FAULT_RECOVER_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault);
#endif

#if (BMS_PROTECT_CELL_DIFF_WARN_ENABLE != 0U)
    /* cell_voltage_diff_waring 置位：最低单体不低于启用电压，且压差达到或超过告警阈值。 */
    BMS_DELAY_SET((data->min_cell_mv >= CELL_DIFF_ENABLE_MIN_CELL_MV) && (data->cell_gap_mv >= CELL_DIFF_WAR_SET_MV),
                  dly_cell_diff_war_set,
                  CELL_DIFF_WAR_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_waring);
    /* cell_voltage_diff_waring 清除：压差回落到告警恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->cell_gap_mv <= CELL_DIFF_WAR_REC_MV,
                    dly_cell_diff_war_recover,
                    CELL_DIFF_WAR_REC_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_waring);
#endif

#if (BMS_PROTECT_CELL_DIFF_PRO_ENABLE != 0U)
    /* cell_voltage_diff_fault 置位：一级保护门限持续成立后置位。 */
    BMS_DELAY_SET((data->min_cell_mv >= CELL_DIFF_ENABLE_MIN_CELL_MV) && (data->cell_gap_mv >= CELL_DIFF_PRO_SET_MV),
                  dly_cell_diff_pro_set,
                  CELL_DIFF_PRO_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault);
    /* cell_voltage_diff_fault 清除：压差回落到保护恢复阈值以下。 */
    BMS_DELAY_CLEAR(data->cell_gap_mv <= CELL_DIFF_PRO_REC_MV,
                    dly_cell_diff_pro_recover,
                    CELL_DIFF_PRO_REC_TIME,
                    gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault);
#endif

#if (BMS_PROTECT_CELL_DIFF_PRO2_ENABLE != 0U)
    /* 二级压差保护使用独立门限和计时器，避免依赖一级标志的触发顺序。 */
    BMS_DELAY_SET((data->min_cell_mv >= CELL_DIFF_ENABLE_MIN_CELL_MV) && (data->cell_gap_mv >= CELL_DIFF_PRO2_SET_MV),
                  dly_cell_diff_pro2_set,
                  CELL_DIFF_PRO2_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2);
    /* 二级为PF锁存：压差恢复不自动清除，只能由通信清故障命令清除。 */
#endif

#if (BMS_PROTECT_CHG_MOS_FAIL_ENABLE != 0U)
    /* CHG MOS失效：有效AFE反馈为断开，但仍有达到或超过配置阈值的充电电流持续达到保护时间。 */
    BMS_DELAY_SET(data->afe_status_valid &&
                      (data->chg_mos_on == false) &&
                      (data->current_ma >= CHG_MOS_FAIL_SET_CUR_MA),
                  dly_chg_mos_fail_set,
                  CHG_MOS_FAIL_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure);
#endif

#if (BMS_PROTECT_DSG_MOS_FAIL_ENABLE != 0U)
    /* DSG MOS失效：有效AFE反馈为断开，但仍有达到或低于负向配置阈值的放电电流持续达到保护时间。 */
    BMS_DELAY_SET(data->afe_status_valid &&
                      (data->dsg_mos_on == false) &&
                      (data->current_ma <= DSG_MOS_FAIL_SET_CUR_MA),
                  dly_dsg_mos_fail_set,
                  DSG_MOS_FAIL_SET_TIME,
                  gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure);
#endif
}
#endif

static void bms_fault_apply_sw_protect_config(void)
{
#if (BMS_SW_PROTECT_ENABLE == 0U)
    /* 软件保护总开关关闭：软件故障图全部清零，后面只合并AFE硬件故障。 */
    memset(&gt_app_bms_fault.tbms_sw_fault_flag,
           0,
           sizeof(gt_app_bms_fault.tbms_sw_fault_flag));
#else
#if (BMS_PROTECT_AFE_FAULT_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault);
#endif
#if (BMS_PROTECT_NTC_FAULT_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault);
#endif

#if (BMS_PROTECT_CELL_OV_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov);
#endif
#if (BMS_PROTECT_CELL_OV_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov);
#endif
#if (BMS_PROTECT_CELL_OV2_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2);
#endif
#if (BMS_PROTECT_PACK_OV_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_war_pov);
#endif
#if (BMS_PROTECT_PACK_OV_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov);
#endif

#if (BMS_PROTECT_CELL_UV_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv);
#endif
#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1);
#endif
#if (BMS_PROTECT_CELL_UV2_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2);
#endif
#if (BMS_PROTECT_CELL_DEEP_UV_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv);
#endif
#if (BMS_PROTECT_PACK_UV_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv);
#endif
#if (BMS_PROTECT_PACK_UV_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv);
#endif

#if (BMS_PROTECT_CHG_OT_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc);
#endif
#if (BMS_PROTECT_CHG_OT_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc);
#endif
#if (BMS_PROTECT_CHG_UT_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_utc);
#endif
#if (BMS_PROTECT_CHG_UT_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc);
#endif

#if (BMS_PROTECT_DSG_OT_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd);
#endif
#if (BMS_PROTECT_DSG_OT_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd);
#endif
#if (BMS_PROTECT_DSG_UT_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_utd);
#endif
#if (BMS_PROTECT_DSG_UT_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd);
#endif

#if (BMS_PROTECT_CHG_OC_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ);
#endif
#if (BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc);
#endif
#if (BMS_PROTECT_CHG_OC_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1);
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2);
#endif
#if (BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc);
#endif
#if (BMS_PROTECT_CHG_OC_LOCK_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock);
#endif

#if (BMS_PROTECT_DSG_OC_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd);
#endif
#if (BMS_PROTECT_DSG_OC_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1);
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2);
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3);
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4);
#endif
#if (BMS_PROTECT_DSG_OC_LOCK_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock);
#endif
#if (BMS_PROTECT_SHORT_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot);
#endif

#if (BMS_PROTECT_CELL_DIFF_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_waring);
#endif
#if (BMS_PROTECT_CELL_DIFF_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault);
#endif
#if (BMS_PROTECT_CELL_DIFF_PRO2_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2);
#endif
#if (BMS_PROTECT_MOS_OT_WARN_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_waring);
#endif
#if (BMS_PROTECT_MOS_OT_PRO_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP);
#endif
#if (BMS_PROTECT_CHG_MOS_FAIL_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure);
#endif
#if (BMS_PROTECT_DSG_MOS_FAIL_ENABLE == 0U)
    BMS_SW_FAULT_CLEAR(gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure);
#endif
#endif
}

void app_bms_fault_update_afe(void)
{
    app_bms_snapshot_t snapshot;

    app_bms_snapshot_read(&snapshot);

    /* 先复制软件故障，再把 AFE 硬件保护位 OR 到最终故障标志中。 */
    memcpy(&gt_app_bms_fault.tbms_fault_flag,
           &gt_app_bms_fault.tbms_sw_fault_flag,
           sizeof(gt_app_bms_fault.tbms_fault_flag));

    /* cell_pro_cov 最终标志：软件单体过压保护或 AFE CHGOV 硬件过压保护任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov =
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov ||
        snapshot.afe_fault.cell_ov;

    /* OCC1最终标志：软件一级保护或AFE充电过流硬件标志任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 =
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 ||
        snapshot.afe_fault.chg_oc1 ||
        snapshot.afe_fault.chg_oc2;

    /* OCC2最终标志只反映软件二级充电过流保护，保持软件分级状态独立。 */
    gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 =
        gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2;

    /* CUV1最终标志：软件一级欠压或AFE DSGUV硬件欠压任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 ||
        snapshot.afe_fault.cell_uv;
    /* CUV2最终标志只反映软件二级欠压，保持两级状态独立。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2;

    /* OCD1最终标志：软件一级保护或AFE一级/汇总过流硬件标志任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 ||
        snapshot.afe_fault.dsg_oc1;
    /* OCD2最终标志：软件二级保护或AFE二级过流硬件标志任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 ||
        snapshot.afe_fault.dsg_oc2;
    /* OCD3/OCD4没有独立AFE等级，只保留软件保护结果。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3;
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4;

    /* pack_shrot 最终标志：软件短路保护或 AFE SC 硬件短路保护任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot =
        gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_B.tdischarge_fault_B.b1pack_shrot ||
        snapshot.afe_fault.short_circuit;

    /* AFE_fault 最终标志：软件 AFE 通信/数据异常或 AFE IICWDGERR 硬件标志任一触发。 */
    gt_app_bms_fault.tbms_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault =
        gt_app_bms_fault.tbms_sw_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault ||
        snapshot.afe_fault.iic_watchdog;
}

static void bms_fault_update_status(void)
{
    const t_bms_fault_flag *fault = &gt_app_bms_fault.tbms_fault_flag;

    /* charge_pro_flag 置位：任一影响充电允许的保护/锁定故障存在。 */
    if ((fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1full_charge_flag != 0U) ||
        (fault->ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault != 0U) ||
        (fault->ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock != 0U))
    {
        gt_app_bms_fault.tbms_fault_stat.u8charge_pro_flag = 1U;
    }
    else
    {
        /* charge_pro_flag 清除：所有充电禁止相关故障均清除。 */
        gt_app_bms_fault.tbms_fault_stat.u8charge_pro_flag = 0U;
    }

    /* discharge_pro_flag 置位：任一影响放电允许的保护/锁定故障存在。 */
    if ((fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 != 0U) ||
        (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 != 0U) ||
        (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U) ||
        (fault->udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd != 0U) ||
        (fault->udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd != 0U) ||
        (fault->udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_shrot != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1predischarge_fault != 0U) ||
        (fault->udischarge_fault_B.tdischarge_fault_B.b1predischarge_lock != 0U))
    {
        gt_app_bms_fault.tbms_fault_stat.u8discharge_pro_flag = 1U;
    }
    else
    {
        /* discharge_pro_flag 清除：所有放电禁止相关故障均清除。 */
        gt_app_bms_fault.tbms_fault_stat.u8discharge_pro_flag = 0U;
    }

    /* general_fault_flag 置位：AFE、压差、MOS、二级硬件、OTP、SCP 熔断等通用故障存在。 */
    if ((fault->ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1NTC_fault != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1hardware_second_pro != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1SCP_FUSE_fusing != 0U))
    {
        gt_app_bms_fault.tbms_fault_stat.u8general_fault_flag = 1U;
    }
    else
    {
        /* general_fault_flag 清除：所有通用故障均清除。 */
        gt_app_bms_fault.tbms_fault_stat.u8general_fault_flag = 0U;
    }

    /* PF_fault_flag置位：压差仅二级保护属于PF；一级保护可恢复，不计入PF。 */
    if ((fault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U) ||
        (fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1SCP_FUSE_fusing != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure != 0U) ||
        (fault->ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure != 0U))
    {
        gt_app_bms_fault.tbms_fault_stat.u8PF_fault_flag = 1U;
    }
    else
    {
        /* PF_fault_flag 清除：所有永久故障来源均清除。 */
        gt_app_bms_fault.tbms_fault_stat.u8PF_fault_flag = 0U;
    }
}

/* 熔断只由严重故障保护条件产生，不参与普通MOS路径判断。 */
static uint8_t bms_fault_efuse_required(const t_bms_fault_flag *fault)
{
    const t_general_fault_A *general_fault;

    if (fault == 0)
    {
        return 0U;
    }

    general_fault = &fault->ugeneral_fault_A.tgeneral_fault_A;
    return ((general_fault->b1SCP_FUSE_fusing != 0U) ||
            (general_fault->b1MOS_OTP_FUSE != 0U) ||
            (general_fault->b1OTD_FUSE != 0U)) ? 1U : 0U;
}

/* 只有命令变化时才操作硬件，避免在10ms故障任务中重复写GPIO。 */
static void bms_fault_update_efuse(void)
{
    uint8_t efuse_cmd;

    efuse_cmd = bms_fault_efuse_required(&gt_app_bms_fault.tbms_fault_flag);
    if ((efuse_cmd == s_bms_efuse_cmd) && (s_bms_efuse_initialized != 0U))
    {
        return;
    }

    app_hw_apply_efuse_set(efuse_cmd);
    s_bms_efuse_cmd = efuse_cmd;
    s_bms_efuse_initialized = 1U;
}

uint8_t app_bms_fault_get_efuse_cmd(void)
{
    return s_bms_efuse_cmd;
}

uint32_t app_bms_fault_get_pf_mask(void)
{
    return bms_fault_collect_pf_mask(&gt_app_bms_fault.tbms_fault_flag);
}

void app_bms_fault_load_shutdown_pf(void)
{
    const bms_fault_pf_shutdown_record_t *record =
        (const bms_fault_pf_shutdown_record_t *)BMS_PF_SHUTDOWN_RECORD_ADDR;
    uint32_t u32PfMask;
    uint32_t u32OverchargeCount;
    uint32_t u32OverdischargeCount;

    taskENTER_CRITICAL();
    gt_app_bms_fault.u32overcharge_count = 0UL;
    gt_app_bms_fault.u32overdischarge_count = 0UL;
    s_bms_pf_record_dirty = 0U;
    s_bms_overcharge_event_previous_active = 0U;
    s_bms_overdischarge_event_previous_active = 0U;
    taskEXIT_CRITICAL();

    if (!bms_fault_pf_record_is_valid(record))
    {
        return;
    }

    memcpy(&u32PfMask,
           ((const uint8_t *)record) + sizeof(record->tag),
           sizeof(u32PfMask));
    memcpy(&u32OverchargeCount,
           ((const uint8_t *)record) + sizeof(record->tag) + sizeof(u32PfMask),
           sizeof(u32OverchargeCount));
    memcpy(&u32OverdischargeCount,
           ((const uint8_t *)record) + sizeof(record->tag) + sizeof(u32PfMask) + sizeof(u32OverchargeCount),
           sizeof(u32OverdischargeCount));

    taskENTER_CRITICAL();
    gt_app_bms_fault.u32overcharge_count =
        (u32OverchargeCount == 0xFFFFFFFFUL) ? 0UL : u32OverchargeCount;
    gt_app_bms_fault.u32overdischarge_count =
        (u32OverdischargeCount == 0xFFFFFFFFUL) ? 0UL : u32OverdischargeCount;
    taskEXIT_CRITICAL();

    bms_fault_restore_pf_mask(u32PfMask);
    bms_fault_apply_sw_protect_config();
    app_bms_fault_update_afe();
    bms_fault_update_status();

    /* 重启恢复的PF已经计入历史次数，设为当前边沿基线，避免再次上电时重复计数。 */
    taskENTER_CRITICAL();
    s_bms_overcharge_event_previous_active =
        bms_fault_is_overcharge_active(&gt_app_bms_fault.tbms_fault_flag);
    s_bms_overdischarge_event_previous_active =
        bms_fault_is_overdischarge_active(&gt_app_bms_fault.tbms_fault_flag);
    taskEXIT_CRITICAL();
}

uint8_t app_bms_fault_save_shutdown_pf(void)
{
    bms_fault_pf_shutdown_record_t record;
    struct _app_bms_fault_t fault_snapshot;
    uint32_t pf_mask;
    uint32_t u32OverchargeCount;
    uint32_t u32OverdischargeCount;

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return 0U;
    }

    /* 快照在Flash互斥锁内采集，避免清PF与后台保存并发时把刚清除的数据写回来。 */
    app_bms_fault_snapshot_read(&fault_snapshot);
    pf_mask = bms_fault_collect_pf_mask(&fault_snapshot.tbms_fault_flag);
    u32OverchargeCount = fault_snapshot.u32overcharge_count;
    u32OverdischargeCount = fault_snapshot.u32overdischarge_count;

    /* 不管当前有没有PF，都先擦掉旧记录；这样PF已经解除后，下次上电不会误恢复旧PF。 */
    if (!d_flash_erase_page(BMS_PF_SHUTDOWN_PAGE_ADDR))
    {
        d_flash_release();
        return 0U;
    }

    if ((pf_mask == 0UL) &&
        (u32OverchargeCount == 0UL) &&
        (u32OverdischargeCount == 0UL))
    {
        d_flash_release();
        taskENTER_CRITICAL();
        if ((gt_app_bms_fault.u32overcharge_count == u32OverchargeCount) &&
            (gt_app_bms_fault.u32overdischarge_count == u32OverdischargeCount))
        {
            s_bms_pf_record_dirty = 0U;
        }
        taskEXIT_CRITICAL();
        return 1U;
    }

    memset(&record, 0xFF, sizeof(record));
    record.tag = BMS_PF_SHUTDOWN_TAG;
    memcpy(((uint8_t *)&record) + sizeof(record.tag), &pf_mask, sizeof(pf_mask));
    memcpy(((uint8_t *)&record) + sizeof(record.tag) + sizeof(pf_mask),
           &fault_snapshot.u32overcharge_count,
           sizeof(fault_snapshot.u32overcharge_count));
    memcpy(((uint8_t *)&record) + sizeof(record.tag) + sizeof(pf_mask) + sizeof(u32OverchargeCount),
           &fault_snapshot.u32overdischarge_count,
           sizeof(fault_snapshot.u32overdischarge_count));

    if (!d_flash_write(BMS_PF_SHUTDOWN_RECORD_ADDR,
                       (const uint8_t *)&record,
                       (uint32_t)sizeof(record)))
    {
        d_flash_release();
        return 0U;
    }

    d_flash_release();

    taskENTER_CRITICAL();
    if ((gt_app_bms_fault.u32overcharge_count == u32OverchargeCount) &&
        (gt_app_bms_fault.u32overdischarge_count == u32OverdischargeCount))
    {
        s_bms_pf_record_dirty = 0U;
    }
    else
    {
        /* Flash写入期间又发生新事件，保留dirty，下一轮保存更新后的次数。 */
        s_bms_pf_record_dirty = 1U;
    }
    taskEXIT_CRITICAL();
    return 1U;
}

void app_bms_fault_storage_task(void)
{
    uint8_t bRecordDirty;

    taskENTER_CRITICAL();
    bRecordDirty = s_bms_pf_record_dirty;
    taskEXIT_CRITICAL();

    if (bRecordDirty != 0U)
    {
        (void)app_bms_fault_save_shutdown_pf();
    }
}

uint8_t app_bms_fault_clear_shutdown_pf(void)
{
    /* 0x68要求同步清除掉电保留的PF记录，擦除成功后上电不会再恢复旧PF。 */
    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return 0U;
    }

    if (!d_flash_erase_page(BMS_PF_SHUTDOWN_PAGE_ADDR))
    {
        d_flash_release();
        return 0U;
    }

    taskENTER_CRITICAL();
    gt_app_bms_fault.u32overcharge_count = 0UL;
    gt_app_bms_fault.u32overdischarge_count = 0UL;
    s_bms_pf_record_dirty = 0U;
    /* 清零与故障图清除之间可能被保护任务抢占；把当前活动状态设为基线避免立刻重计。 */
    s_bms_overcharge_event_previous_active =
        bms_fault_is_overcharge_active(&gt_app_bms_fault.tbms_fault_flag);
    s_bms_overdischarge_event_previous_active =
        bms_fault_is_overdischarge_active(&gt_app_bms_fault.tbms_fault_flag);
    taskEXIT_CRITICAL();

    d_flash_release();
    return 1U;
}

void app_bms_fault_clear_all(void)
{
    uint16_t old_ocd_count;
    uint8_t old_ocd_lock;
    uint8_t old_ocd_warning;

    /* 清除全部运行态故障图；故障判断宏和保护配置保持不变。 */
    taskENTER_CRITICAL();
    old_ocd_count = s_bms_ocd_recover_count;
    old_ocd_lock = (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock;
    old_ocd_warning = (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd;
    memset(&gt_app_bms_fault.tbms_sw_fault_flag,
           0,
           sizeof(gt_app_bms_fault.tbms_sw_fault_flag));
    memset(&gt_app_bms_fault.tbms_fault_flag,
           0,
           sizeof(gt_app_bms_fault.tbms_fault_flag));
    memset(&gt_app_bms_fault.tbms_fault_stat,
           0,
           sizeof(gt_app_bms_fault.tbms_fault_stat));
    /* 清除放电过流恢复计时残留，避免命令清除后沿用旧计时状态。 */
    s_bms_ocd_recover_count = 0U;
    /* 清除命令以当前AFE短路状态建立基线，持续置位的同一次故障不会立刻重复计数。 */
    s_bms_short_event_previous_active =
        (s_bms_fault_last_data.afe_status_valid && s_bms_fault_last_data.afe_short_circuit) ? 1U : 0U;
    taskEXIT_CRITICAL();

    if (old_ocd_warning != 0U)
    {
        bms_fault_rtt_ocd_warning("CLEAR_CMD");
    }
    bms_fault_rtt_ocd_reset("clear_cmd", old_ocd_count, old_ocd_lock);
}

void app_bms_fault_merge(void)
{
    static uint8_t bFaultSnapshotInitialized = 0U;
    /* 当前/历史故障按字节比较，避免逐个故障bit手写判断，后续新增故障位也能自动触发LOG。 */
    const uint8_t *pu8CurrentFaultBytes = (const uint8_t *)&gt_app_bms_fault.tbms_fault_flag;
    const uint8_t *pu8PreviousFaultBytes = (const uint8_t *)&gt_app_bms_fault.tsave_bms_fault_flag;
    uint8_t bFaultSetEvent = 0U;
    uint8_t bFaultClearEvent = 0U;
    uint16_t u16FaultByteIndex;

    if (bFaultSnapshotInitialized == 0U)
    {
        /* 第一次只建立故障镜像，不保存LOG，避免上电默认状态产生误记录。 */
        bFaultSnapshotInitialized = 1U;
        memcpy(&gt_app_bms_fault.tsave_bms_fault_flag,
               &gt_app_bms_fault.tbms_fault_flag,
               sizeof(gt_app_bms_fault.tbms_fault_flag));
        return;
    }

    if (memcmp(&gt_app_bms_fault.tbms_fault_flag,
               &gt_app_bms_fault.tsave_bms_fault_flag,
               sizeof(gt_app_bms_fault.tsave_bms_fault_flag)) != 0)
    {
        for (u16FaultByteIndex = 0U; u16FaultByteIndex < (uint16_t)sizeof(gt_app_bms_fault.tbms_fault_flag); u16FaultByteIndex++)
        {
            /* 当前为1、上次为0，说明这一字节里至少有一个故障刚刚置位。 */
            if ((pu8CurrentFaultBytes[u16FaultByteIndex] & (uint8_t)(~pu8PreviousFaultBytes[u16FaultByteIndex])) != 0U)
            {
                bFaultSetEvent = 1U;
            }

            /* 当前为0、上次为1，说明这一字节里至少有一个故障刚刚清除。 */
            if (((uint8_t)(~pu8CurrentFaultBytes[u16FaultByteIndex]) & pu8PreviousFaultBytes[u16FaultByteIndex]) != 0U)
            {
                bFaultClearEvent = 1U;
            }
        }

        if ((bFaultSetEvent != 0U) && (bFaultClearEvent != 0U))
        {
            /* 同一轮里既有故障产生也有故障恢复，保存完整快照，方便后续追溯。 */
            app_log_request_snapshot(LOG_TRIGGER_FAULT_BOTH);
        }
        else if (bFaultSetEvent != 0U)
        {
            /* 故障置位时保存一次当前电压、电流、温度、MOS状态和故障图。 */
            app_log_request_snapshot(LOG_TRIGGER_FAULT_SET);
        }
        else if (bFaultClearEvent != 0U)
        {
            /* 故障清除时也保存一次，记录恢复时刻的系统状态。 */
            app_log_request_snapshot(LOG_TRIGGER_FAULT_CLEAR);
        }

        memcpy(&gt_app_bms_fault.tsave_bms_fault_flag,
               &gt_app_bms_fault.tbms_fault_flag,
               sizeof(gt_app_bms_fault.tbms_fault_flag));
    }
}

static void bms_fault_update_ocd_lock_idle_recovery(const bms_fault_data_t *data)
{
#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U) && (BMS_PROTECT_DSG_OC_LOCK_ENABLE != 0U)
    BMS_DECLARE_DELAY_VARIABLE(dly_ocd_lock_idle_recover);

    /* 从过流/短路共用锁置位后开始计30分钟；解锁、放电或数据无效均重新计时。 */
    if ((data != 0) && data->data_ready && (data->discharging == false) &&
        (gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U))
    {
        if (d_timer_delay_check(true,
                                &dly_ocd_lock_idle_recover,
                                OCD_LOCK_IDLE_RECOVER_TIME) == Ok)
        {
            /* 静置恢复：清除放电过流累计次数和锁定位，允许后续放电重新累计。 */
            bms_fault_rtt_ocd_reset(
                "idle30m",
                s_bms_ocd_recover_count,
                (uint8_t)gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock);
            s_bms_ocd_recover_count = 0U;
            gt_app_bms_fault.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock = 0U;
        }
    }
    else
    {
        (void)d_timer_delay_check(false,
                                  &dly_ocd_lock_idle_recover,
                                  OCD_LOCK_IDLE_RECOVER_TIME);
    }
#else
    (void)data;
#endif
}

static void bms_fault_update_voltage_event_counts(void)
{
    const t_bms_fault_flag *fault = &gt_app_bms_fault.tbms_fault_flag;
    uint8_t current_overcharge_active;
    uint8_t current_overdischarge_active;
    uint8_t previous_overcharge_active;
    uint8_t previous_overdischarge_active;

    taskENTER_CRITICAL();
    current_overcharge_active = bms_fault_is_overcharge_active(fault);
    current_overdischarge_active = bms_fault_is_overdischarge_active(fault);
    previous_overcharge_active = s_bms_overcharge_event_previous_active;
    previous_overdischarge_active = s_bms_overdischarge_event_previous_active;

    /* 同一方向任一保护从无到有计一次；持续保护或升级到更高等级不重复累计。 */
    if ((current_overcharge_active != 0U) && (previous_overcharge_active == 0U) &&
        (gt_app_bms_fault.u32overcharge_count < 0xFFFFFFFEUL))
    {
        gt_app_bms_fault.u32overcharge_count++;
        s_bms_pf_record_dirty = 1U;
    }
    if ((current_overdischarge_active != 0U) && (previous_overdischarge_active == 0U) &&
        (gt_app_bms_fault.u32overdischarge_count < 0xFFFFFFFEUL))
    {
        gt_app_bms_fault.u32overdischarge_count++;
        s_bms_pf_record_dirty = 1U;
    }

    s_bms_overcharge_event_previous_active = current_overcharge_active;
    s_bms_overdischarge_event_previous_active = current_overdischarge_active;
    taskEXIT_CRITICAL();
}

void app_bms_fault_task(uint8_t u8device_id)
{
    bms_fault_data_t data;

    (void)u8device_id;

    bms_fault_collect_data(&data);
    s_bms_fault_last_data = data;
    gt_app_bms_fault.u8data_init_flag = (uint8_t)data.data_ready;

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE != 0U)
    /* 软件阈值保护总开关开启时，才进入各类软件保护判断。 */
    #if (BMS_PROTECT_AFE_FAULT_ENABLE != 0U)
    bms_fault_update_afe_fault(&data);
    #endif
    if (data.data_ready != false)
    {
        /* 各子保护在函数内部按对应开关继续编译裁剪。 */
        bms_fault_update_voltage_faults(&data);
        bms_fault_update_temp_faults(&data);
        bms_fault_update_current_faults(&data);
        bms_fault_update_general_faults(&data);
    }
#else
    /* 关闭软件保护时，不运行任何软件阈值判断，只清掉历史软件故障图。 */
    memset(&gt_app_bms_fault.tbms_sw_fault_flag,
           0,
           sizeof(gt_app_bms_fault.tbms_sw_fault_flag));
#endif

    bms_fault_update_dsg_oc_short_lock(&data);
    bms_fault_update_ocd_lock_idle_recovery(&data);
    bms_fault_update_load_removed_recovery(&data);
    bms_fault_update_afe_recovery(&data);
    bms_fault_apply_sw_protect_config();
    app_bms_fault_update_afe();
    bms_fault_update_status();
    bms_fault_update_voltage_event_counts();
    app_bms_fault_merge();
    bms_fault_update_efuse();
}
