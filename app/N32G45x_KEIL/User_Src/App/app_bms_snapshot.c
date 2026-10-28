/*
 * 文件作用：
 *   统一业务快照层，把当前项目分散在AFE、Board和功率管理模块中的硬件状态整理成
 *   app_bms_snapshot_t，供故障、MOS、LOG、通信和低功耗等业务模块统一读取。
 *
 * 主要用途：
 *   1. 隔离业务模块和底层硬件数据来源。
 *   2. 避免业务模块直接读取 afe、gt_app_h_power_manage 等全局对象。
 *   3. 为后续模块迁移提供统一输入结构。
 *
 * 适用范围：
 *   仅适用于当前 APP 工程的业务数据适配层。这里可以依赖 HSI_afe、Board 和现有业务对象，
 *   但不写保护判断、不写MOS策略、不写通信协议。
 *
 * 调用关系：
 *   DataHandle_Task 每10ms调用 app_bms_snapshot_update()；
 *   上层业务模块通过 app_bms_snapshot_read() 读取最近一次完整快照。
 *
 * 注意事项：
 *   本文件是过渡期适配层，允许读取现有全局对象；后续业务模块切到快照后，再逐步收敛
 *   AFE和功率管理模块的公开接口。
 */
#include "App/app_bms_snapshot.h"
#include "App/app_bms_fault.h"
#include "App/app_bms_obc.h"
#include "App/app_cell_topology.h"
#include "App/app_h_power_manage.h"
#include "App/app_input_signals.h"
#include "App/app_mcu_ntc.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "FreeRTOS.h"
#include "HSI_afe/hsi_afe.h"
#include "app_soc.h"
#include "app_sop.h"
#include "task.h"
#include <string.h>

app_bms_snapshot_t gt_app_bms_snapshot;

static uint8_t app_bms_snapshot_cell_count(void)
{
    if (gt_hsi_afe.cells == 0U)
    {
        return APP_BMS_CELL_COUNT;
    }

    if (gt_hsi_afe.cells > APP_BMS_CELL_COUNT)
    {
        return APP_BMS_CELL_COUNT;
    }

    return gt_hsi_afe.cells;
}

static int16_t app_bms_snapshot_temp_0p1c_to_c(int16_t s16Temperature0p1c)
{
    if (s16Temperature0p1c >= 0)
    {
        return (int16_t)((s16Temperature0p1c + 5) / 10);
    }

    return (int16_t)((s16Temperature0p1c - 5) / 10);
}

static void app_bms_snapshot_update_ntc(app_bms_snapshot_t *ptSnapshot)
{
    const app_mcu_ntc_t *ptMcuNtc;

    memset(&ptSnapshot->ntc, 0, sizeof(ptSnapshot->ntc));
    ptSnapshot->ntc.afe_temp_0p1c =
        (gt_hsi_afe.temp[0] != INVALID_TEMPERATURE) ?
        (int16_t)(gt_hsi_afe.temp[0] * 10) : APP_BMS_TEMP_INVALID_VALUE;

    if (gt_hsi_afe.temp[0] != INVALID_TEMPERATURE)
    {
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_AFE;
    }
    if (gt_hsi_afe.ntc_status == HSI_AFE_DVC1124_NTC_STATUS_ELECTRICAL_FAULT)
    {
        ptSnapshot->ntc.fault_mask |= APP_BMS_NTC_FAULT_AFE;
    }

    ptMcuNtc = app_mcu_ntc_get();
    if (ptMcuNtc == 0)
    {
        return;
    }

    ptSnapshot->ntc.cell_ntc1_0p1c = ptMcuNtc->ntc1_0p1c;
    ptSnapshot->ntc.cell_ntc2_0p1c = ptMcuNtc->ntc2_0p1c;
    ptSnapshot->ntc.mos_temp_0p1c = ptMcuNtc->mos_0p1c;
    ptSnapshot->ntc.ptc_temp_0p1c = ptMcuNtc->ptc_0p1c;
    ptSnapshot->ntc.mcu_temp_0p1c = ptMcuNtc->mcu_temp_0p1c;
    ptSnapshot->ntc.cell_ntc1_raw = ptMcuNtc->ntc1_raw;
    ptSnapshot->ntc.cell_ntc2_raw = ptMcuNtc->ntc2_raw;
    ptSnapshot->ntc.mos_raw = ptMcuNtc->mos_raw;
    ptSnapshot->ntc.ptc_raw = ptMcuNtc->ptc_raw;
    ptSnapshot->ntc.mcu_temp_raw = ptMcuNtc->mcu_temp_raw;
    ptSnapshot->ntc.cell_ntc1_res_ohm = ptMcuNtc->ntc1_res_ohm;
    ptSnapshot->ntc.cell_ntc2_res_ohm = ptMcuNtc->ntc2_res_ohm;
    ptSnapshot->ntc.mos_res_ohm = ptMcuNtc->mos_res_ohm;
    ptSnapshot->ntc.ptc_res_ohm = ptMcuNtc->ptc_res_ohm;

    if ((ptMcuNtc->valid_mask & APP_MCU_NTC_VALID_NTC1) != 0U)
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_CELL1;
    if ((ptMcuNtc->valid_mask & APP_MCU_NTC_VALID_NTC2) != 0U)
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_CELL2;
    if ((ptMcuNtc->valid_mask & APP_MCU_NTC_VALID_MOS) != 0U)
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_MOS;
    if ((ptMcuNtc->valid_mask & APP_MCU_NTC_VALID_PTC) != 0U)
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_PTC;
    if ((ptMcuNtc->valid_mask & APP_MCU_NTC_VALID_MCU_TEMP) != 0U)
        ptSnapshot->ntc.valid_mask |= APP_BMS_NTC_VALID_MCU;

    if ((ptMcuNtc->fault_mask & APP_MCU_NTC_FAULT_NTC1) != 0U)
        ptSnapshot->ntc.fault_mask |= APP_BMS_NTC_FAULT_CELL1;
    if ((ptMcuNtc->fault_mask & APP_MCU_NTC_FAULT_NTC2) != 0U)
        ptSnapshot->ntc.fault_mask |= APP_BMS_NTC_FAULT_CELL2;
    if ((ptMcuNtc->fault_mask & APP_MCU_NTC_FAULT_MOS) != 0U)
        ptSnapshot->ntc.fault_mask |= APP_BMS_NTC_FAULT_MOS;
    if ((ptMcuNtc->fault_mask & APP_MCU_NTC_FAULT_PTC) != 0U)
        ptSnapshot->ntc.fault_mask |= APP_BMS_NTC_FAULT_PTC;

    ptSnapshot->ntc.short_mask =
        ((ptMcuNtc->short_mask & APP_MCU_NTC_FAULT_NTC1) != 0U ? APP_BMS_NTC_FAULT_CELL1 : 0U) |
        ((ptMcuNtc->short_mask & APP_MCU_NTC_FAULT_NTC2) != 0U ? APP_BMS_NTC_FAULT_CELL2 : 0U) |
        ((ptMcuNtc->short_mask & APP_MCU_NTC_FAULT_MOS) != 0U ? APP_BMS_NTC_FAULT_MOS : 0U) |
        ((ptMcuNtc->short_mask & APP_MCU_NTC_FAULT_PTC) != 0U ? APP_BMS_NTC_FAULT_PTC : 0U);
    ptSnapshot->ntc.open_mask =
        ((ptMcuNtc->open_mask & APP_MCU_NTC_FAULT_NTC1) != 0U ? APP_BMS_NTC_FAULT_CELL1 : 0U) |
        ((ptMcuNtc->open_mask & APP_MCU_NTC_FAULT_NTC2) != 0U ? APP_BMS_NTC_FAULT_CELL2 : 0U) |
        ((ptMcuNtc->open_mask & APP_MCU_NTC_FAULT_MOS) != 0U ? APP_BMS_NTC_FAULT_MOS : 0U) |
        ((ptMcuNtc->open_mask & APP_MCU_NTC_FAULT_PTC) != 0U ? APP_BMS_NTC_FAULT_PTC : 0U);
    ptSnapshot->ntc.all_valid =
        ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_EXTERNAL) == APP_BMS_NTC_VALID_EXTERNAL) ? 1U : 0U;
}

static void app_bms_snapshot_update_cells(app_bms_snapshot_t *ptSnapshot)
{
    uint8_t u8CellIndex;
    uint16_t u16CellVoltageMv;

    ptSnapshot->cell_count = app_bms_snapshot_cell_count();
    ptSnapshot->valid_cells = 0U;
    ptSnapshot->max_cell_mv = 0U;
    ptSnapshot->min_cell_mv = 0xFFFFU;
    ptSnapshot->max_cell_num = 0U;
    ptSnapshot->min_cell_num = 0U;

    for (u8CellIndex = 0U; u8CellIndex < APP_BMS_CELL_COUNT; u8CellIndex++)
    {
        ptSnapshot->cell_mv[u8CellIndex] = 0U;
    }

    for (u8CellIndex = 0U; u8CellIndex < ptSnapshot->cell_count; u8CellIndex++)
    {
        u16CellVoltageMv = app_bms_topology_get_cell_voltage_mv(gt_hsi_afe.vol,
                                                                 u8CellIndex,
                                                                 ptSnapshot->cell_count);
        if (u16CellVoltageMv != 0U)
        {
            u16CellVoltageMv = app_param_apply_cell_voltage_offset(u8CellIndex,
                                                                    u16CellVoltageMv);
        }
        ptSnapshot->cell_mv[u8CellIndex] = u16CellVoltageMv;

        if (u16CellVoltageMv != 0U)
        {
            ptSnapshot->valid_cells++;
        }

        if (u16CellVoltageMv > ptSnapshot->max_cell_mv)
        {
            ptSnapshot->max_cell_mv = u16CellVoltageMv;
            ptSnapshot->max_cell_num = u8CellIndex;
        }

        if (u16CellVoltageMv < ptSnapshot->min_cell_mv)
        {
            ptSnapshot->min_cell_mv = u16CellVoltageMv;
            ptSnapshot->min_cell_num = u8CellIndex;
        }
    }

    ptSnapshot->data_ready = ((gt_hsi_afe.data_ready != 0U) &&
                              (ptSnapshot->cell_count > 0U) &&
                              (ptSnapshot->valid_cells == ptSnapshot->cell_count)) ? 1U : 0U;

    if (ptSnapshot->data_ready == 0U)
    {
        ptSnapshot->min_cell_mv = 0U;
        ptSnapshot->cell_gap_mv = 0U;
    }
    else
    {
        ptSnapshot->cell_gap_mv = (uint16_t)(ptSnapshot->max_cell_mv - ptSnapshot->min_cell_mv);
    }
}

static uint32_t app_bms_snapshot_sum_cell_voltage(const app_bms_snapshot_t *ptSnapshot)
{
    uint8_t u8CellIndex;
    uint32_t u32PBatVoltageMv = 0U;

    if ((ptSnapshot->data_ready == 0U) ||
        (ptSnapshot->cell_count == 0U) ||
        (ptSnapshot->cell_count > APP_BMS_CELL_COUNT) ||
        (ptSnapshot->valid_cells != ptSnapshot->cell_count))
    {
        return 0U;
    }

    for (u8CellIndex = 0U; u8CellIndex < ptSnapshot->cell_count; u8CellIndex++)
    {
        u32PBatVoltageMv += ptSnapshot->cell_mv[u8CellIndex];
    }

    return u32PBatVoltageMv;
}

static void app_bms_snapshot_update_current(app_bms_snapshot_t *ptSnapshot)
{
    int32_t s32Current10ma;

    /* current_ma给通信和上层显示使用，保留K/B校准后的实际小电流，不套用1A零死区。 */
    ptSnapshot->current_ma = gt_hsi_afe.curr_calibrated;
    ptSnapshot->afe_current_update_seq = gt_hsi_afe.current_update_seq;
    /* current_10ma给保护和充放电方向判断使用，继续采用零死区后的业务电流。 */
    s32Current10ma = gt_hsi_afe.curr_real / 10;

    if (s32Current10ma > 32767)
    {
        s32Current10ma = 32767;
    }
    else if (s32Current10ma < -32768)
    {
        s32Current10ma = -32768;
    }

    ptSnapshot->current_10ma = (int16_t)s32Current10ma;
    ptSnapshot->charging = (ptSnapshot->current_10ma > 0) ? 1U : 0U;
    ptSnapshot->discharging = (ptSnapshot->current_10ma < 0) ? 1U : 0U;
}

static void app_bms_snapshot_init_temp_values(app_bms_snapshot_t *ptSnapshot)
{
    uint8_t u8TemperatureIndex;

    ptSnapshot->temp_count = 0U;
    ptSnapshot->temp_valid_mask = 0U;
    ptSnapshot->max_temp_c = APP_BMS_TEMP_INVALID_VALUE;
    ptSnapshot->min_temp_c = APP_BMS_TEMP_INVALID_VALUE;
    ptSnapshot->max_temp_num = 0U;
    ptSnapshot->min_temp_num = 0U;

    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < APP_BMS_MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        ptSnapshot->temp_c[u8TemperatureIndex] = APP_BMS_TEMP_INVALID_VALUE;
        ptSnapshot->temp_0p1c[u8TemperatureIndex] = APP_BMS_TEMP_INVALID_VALUE;
    }
}

static void app_bms_snapshot_update_temps(app_bms_snapshot_t *ptSnapshot)
{
    uint8_t u8TemperatureIndex;
    int16_t s16TemperatureC;
    uint8_t u8ValidTemperatureMask;

    app_bms_snapshot_init_temp_values(ptSnapshot);
    /* AFE_NTC只有电压/阻值越界才算NTC电气异常，IIC失败由AFE故障链路单独处理。 */
    ptSnapshot->ntc_abnormal_count =
        ((ptSnapshot->ntc.fault_mask & APP_BMS_NTC_FAULT_AFE) != 0U) ? 1U : 0U;

    /* 四路MCU_NTC按通道统计，同一通道即使同时短路和采样失败也只计一次。 */
    if ((ptSnapshot->ntc.fault_mask & APP_BMS_NTC_FAULT_CELL1) != 0U)
    {
        ptSnapshot->ntc_abnormal_count++;
    }
    if ((ptSnapshot->ntc.fault_mask & APP_BMS_NTC_FAULT_CELL2) != 0U)
    {
        ptSnapshot->ntc_abnormal_count++;
    }
    if ((ptSnapshot->ntc.fault_mask & APP_BMS_NTC_FAULT_MOS) != 0U)
    {
        ptSnapshot->ntc_abnormal_count++;
    }
    if ((ptSnapshot->ntc.fault_mask & APP_BMS_NTC_FAULT_PTC) != 0U)
    {
        ptSnapshot->ntc_abnormal_count++;
    }

    /*
     * 快照中的电芯温度统一整理成MAX_TEMP_NUM路：
     *   MAX_TEMP_NUM = AFE_NTC_NUM + MCU_NTC_NUM。
     *   1. AFE_NTC：DVC1124内部温度通道，单位整数℃。
     *   2. MCU_NTC1：原理图CELL_NTC1，MCU ADC采样，模块输出0.1℃。
     *   3. MCU_NTC2：原理图CELL_NTC2，MCU ADC采样，模块输出0.1℃。
     * MOS_NTC和PTC_NTC不是电芯温度，不放进这个电芯温度数组。
     */
    if ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_AFE) != 0U)
    {
        ptSnapshot->temp_c[0] = app_bms_snapshot_temp_0p1c_to_c(ptSnapshot->ntc.afe_temp_0p1c);
        ptSnapshot->temp_0p1c[0] = ptSnapshot->ntc.afe_temp_0p1c;
        ptSnapshot->temp_valid_mask |= 0x01U;
        ptSnapshot->temp_count++;
    }

    if ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_CELL1) != 0U)
    {
        ptSnapshot->temp_c[1] = app_bms_snapshot_temp_0p1c_to_c(ptSnapshot->ntc.cell_ntc1_0p1c);
        ptSnapshot->temp_0p1c[1] = ptSnapshot->ntc.cell_ntc1_0p1c;
        ptSnapshot->temp_valid_mask |= 0x02U;
        ptSnapshot->temp_count++;
    }

    if ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_CELL2) != 0U)
    {
        ptSnapshot->temp_c[2] = app_bms_snapshot_temp_0p1c_to_c(ptSnapshot->ntc.cell_ntc2_0p1c);
        ptSnapshot->temp_0p1c[2] = ptSnapshot->ntc.cell_ntc2_0p1c;
        ptSnapshot->temp_valid_mask |= 0x04U;
        ptSnapshot->temp_count++;
    }

    if (ptSnapshot->temp_count == 0U)
    {
        return;
    }

    u8ValidTemperatureMask = ptSnapshot->temp_valid_mask;
    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < APP_BMS_MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        if ((u8ValidTemperatureMask & (uint8_t)(1U << u8TemperatureIndex)) != 0U)
        {
            ptSnapshot->max_temp_c = ptSnapshot->temp_c[u8TemperatureIndex];
            ptSnapshot->min_temp_c = ptSnapshot->temp_c[u8TemperatureIndex];
            ptSnapshot->max_temp_num = u8TemperatureIndex;
            ptSnapshot->min_temp_num = u8TemperatureIndex;
            break;
        }
    }

    for (u8TemperatureIndex = (uint8_t)(u8TemperatureIndex + 1U);
         u8TemperatureIndex < APP_BMS_MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        if ((u8ValidTemperatureMask & (uint8_t)(1U << u8TemperatureIndex)) == 0U)
        {
            continue;
        }

        s16TemperatureC = ptSnapshot->temp_c[u8TemperatureIndex];

        if (s16TemperatureC > ptSnapshot->max_temp_c)
        {
            ptSnapshot->max_temp_c = s16TemperatureC;
            ptSnapshot->max_temp_num = u8TemperatureIndex;
        }

        if (s16TemperatureC < ptSnapshot->min_temp_c)
        {
            ptSnapshot->min_temp_c = s16TemperatureC;
            ptSnapshot->min_temp_num = u8TemperatureIndex;
        }
    }
}

static void app_bms_snapshot_update_afe_fault(app_bms_snapshot_t *snapshot)
{
    snapshot->afe_status_valid = gt_hsi_afe.status_valid;
    snapshot->afe_fault.cell_ov = gt_hsi_afe.status.bits.chgov;
    snapshot->afe_fault.cell_uv = gt_hsi_afe.status.bits.dsguv;
    snapshot->afe_fault.chg_oc1 = gt_hsi_afe.status.bits.chgoc;
    snapshot->afe_fault.chg_oc2 = gt_hsi_afe.status.bits.chgoc2;
    snapshot->afe_fault.dsg_oc1 = gt_hsi_afe.status.bits.dsgoc;
    snapshot->afe_fault.dsg_oc2 = gt_hsi_afe.status.bits.dsgoc2;
    snapshot->afe_fault.short_circuit = gt_hsi_afe.status.bits.sc;
    snapshot->afe_fault.iic_watchdog = gt_hsi_afe.status.bits.iicwdgerr;
    snapshot->afe_fault.ntc_iic_fault =
        (gt_hsi_afe.ntc_status == HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT) ? 1U : 0U;
    snapshot->afe_fault.charge_detect = gt_hsi_afe.status.bits.chgdet;
}

static void app_bms_snapshot_fill_fault_image(app_bms_fault_image_t *image,
                                               const t_bms_fault_flag *fault,
                                               const t_bms_fault_stat *stat)
{
    memset(image, 0, sizeof(*image));
    if (fault == 0)
    {
        return;
    }

    image->detail = *fault;

    if (stat != 0)
    {
        image->charge_pro = stat->u8charge_pro_flag;
        image->discharge_pro = stat->u8discharge_pro_flag;
        image->general_fault = stat->u8general_fault_flag;
        image->pf_fault = stat->u8PF_fault_flag;
    }
}

static void app_bms_snapshot_update_fault(app_bms_snapshot_t *snapshot)
{
    struct _app_bms_fault_t tFaultSnapshot;

    app_bms_fault_snapshot_read(&tFaultSnapshot);

    app_bms_snapshot_fill_fault_image(&snapshot->fault.software,
                                      &tFaultSnapshot.tbms_sw_fault_flag,
                                      0);
    app_bms_snapshot_fill_fault_image(&snapshot->fault.active,
                                      &tFaultSnapshot.tbms_fault_flag,
                                      &tFaultSnapshot.tbms_fault_stat);
    app_bms_snapshot_fill_fault_image(&snapshot->fault.saved,
                                      &tFaultSnapshot.tsave_bms_fault_flag,
                                      0);
}

static void app_bms_snapshot_update_mos(app_bms_snapshot_t *snapshot)
{
    snapshot->mos.chg_status = gt_app_h_power_manage.tstat.u8chg_status;
    snapshot->mos.dsg_status = gt_app_h_power_manage.tstat.u8dsg_path_allowed;
    snapshot->mos.chg_mos_state = gt_hsi_afe.status.bits.chgmos;
    snapshot->mos.dsg_mos_state = gt_hsi_afe.status.bits.dsgmos;
    snapshot->mos.pre_mos_state = gt_app_h_power_manage.tstat.u8pdsgmos_state;
    snapshot->mos.chg_mos_cmd = gt_app_h_power_manage.tcmd.u8cmos_ctl;
    snapshot->mos.dsg_mos_cmd = gt_app_h_power_manage.tcmd.u8dmos_ctl;
    snapshot->mos.pre_mos_cmd = gt_app_h_power_manage.tcmd.u8pdmos_ctl;
    snapshot->mos.efuse_cmd = app_bms_fault_get_efuse_cmd();
    snapshot->mos.pdsg_step = gt_app_h_power_manage.tpdsg.u8step;
}

static void app_bms_snapshot_update_sop(app_bms_snapshot_t *ptSnapshot)
{
    ptSnapshot->sop.s16MaxDischargeCurrent10mA =
        gt_app_sop.s16max_discharge_current01A;
    ptSnapshot->sop.s16MaxChargeCurrent10mA =
        gt_app_sop.s16max_charge_current01A;
    ptSnapshot->sop.u16MaxRechargeCurrent10mA =
        gt_app_sop.u16max_recharge_current01A;
    ptSnapshot->sop.u32MaxChargeVoltageMv =
        APP_PARAM_MAX_CHARGE_VOLTAGE_MV;
}

static void app_bms_snapshot_update_signal(app_bms_snapshot_t *snapshot)
{
    snapshot->signal.input = gt_app_input_signals;
}

void app_bms_snapshot_init(void)
{
    memset(&gt_app_bms_snapshot, 0, sizeof(gt_app_bms_snapshot));
    app_bms_snapshot_init_temp_values(&gt_app_bms_snapshot);
}

void app_bms_snapshot_update(void)
{
    app_bms_snapshot_t next_snapshot;
    app_bms_obc_state_t obc_state;

    /* OBC状态独立于AFE锁刷新；即使本轮AFE快照被跳过也保持及时可读。 */
    app_bms_obc_read(&obc_state);
    taskENTER_CRITICAL();
    gt_app_bms_snapshot.obc = obc_state;
    taskEXIT_CRITICAL();

    if (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
    {
        return;
    }

    /* 只刷新本轮硬件采样，保留SOC等跨周期业务状态。 */
    taskENTER_CRITICAL();
    next_snapshot = gt_app_bms_snapshot;
    taskEXIT_CRITICAL();

    app_bms_snapshot_update_cells(&next_snapshot);
    next_snapshot.bat_mv = app_bms_snapshot_sum_cell_voltage(&next_snapshot);
    next_snapshot.vbat_mv = gt_hsi_afe.vbat;
    app_bms_snapshot_update_current(&next_snapshot);
    app_bms_snapshot_update_ntc(&next_snapshot);
    app_bms_snapshot_update_temps(&next_snapshot);
    app_bms_snapshot_update_afe_fault(&next_snapshot);
    app_bms_snapshot_update_fault(&next_snapshot);
    app_bms_snapshot_update_mos(&next_snapshot);
    app_bms_snapshot_update_sop(&next_snapshot);
    app_bms_snapshot_update_signal(&next_snapshot);

    taskENTER_CRITICAL();
    gt_app_bms_snapshot = next_snapshot;
    taskEXIT_CRITICAL();

    hsi_afe_release();
}

void app_bms_snapshot_update_soc(void)
{
    app_bms_soc_state_t soc;

    /* SOC算法仍保留现有gt_app_soc作为唯一运行存储；这里仅做统一快照镜像。 */
    soc.tsoc_accumulate_cap_data.s32total_accumulate_charge_cap =
        gt_app_soc.tsoc_accumulate_cap_data.s32total_accumulate_charge_cap;
    soc.tsoc_accumulate_cap_data.s32total_accumulate_discharge_cap =
        gt_app_soc.tsoc_accumulate_cap_data.s32total_accumulate_discharge_cap;
    soc.tsoc_accumulate_cap_data.s32single_accumulate_charge_cap =
        gt_app_soc.tsoc_accumulate_cap_data.s32single_accumulate_charge_cap;
    soc.tsoc_accumulate_cap_data.s32single_accumulate_discharge_cap =
        gt_app_soc.tsoc_accumulate_cap_data.s32single_accumulate_discharge_cap;
    soc.tsoc_accumulate_cap_data.dChgDsgAccumCap =
        gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap;
    soc.tsoc_compensate_para.s8stock_soc = gt_app_soc.tsoc_compensate_para.s8stock_soc;
    soc.tsoc_compensate_para.s8real_soc = gt_app_soc.tsoc_compensate_para.s8real_soc;
    soc.tsoc_capacity_data.s32real_cap_val = gt_app_soc.tsoc_capacity_data.s32real_cap_val;
    soc.tsoc_capacity_data.s32display_cap_val = gt_app_soc.tsoc_capacity_data.s32display_cap_val;
    soc.tsoc_capacity_data.s32cap_cnt = gt_app_soc.tsoc_capacity_data.s32cap_cnt;
    soc.tsoc_fcc_data.u8handle_soc = gt_app_soc.tsoc_fcc_data.u8handle_soc;
    soc.tsoc_fcc_data.u8fcc_condition_flag = gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag;
    soc.tsoc_fcc_update.u8SocInit = gt_app_soc.tsoc_fcc_update.u8SocInit;
    memcpy(soc.tsoc_fcc_update.u16FullChgCapBuf,
           gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf,
           sizeof(soc.tsoc_fcc_update.u16FullChgCapBuf));
    soc.tsoc_fcc_update.dCycleCap = gt_app_soc.tsoc_fcc_update.dCycleCap;
    soc.s32battery_full_cap = gt_app_soc.s32battery_full_cap;
    soc.u8display_soc = gt_app_soc.u8display_soc;
    soc.u8display_soh = gt_app_soc.u8display_soh;
    soc.u8soc_fcc_flag = gt_app_soc.u8soc_fcc_flag;
    soc.s32remain_cap = gt_app_soc.s32remain_cap;
    soc.u32cycle_index = gt_app_soc.u32cycle_index;
    soc.u32tim_250ms_cnt = gt_app_soc.u32tim_250ms_cnt;

    taskENTER_CRITICAL();
    gt_app_bms_snapshot.soc = soc;
    taskEXIT_CRITICAL();
}

void app_bms_snapshot_set_work_status(uint8_t bms_work_status)
{
    taskENTER_CRITICAL();
    gt_app_bms_snapshot.bms_work_status = bms_work_status;
    taskEXIT_CRITICAL();
}

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot)
{
    if (snapshot == 0)
    {
        return;
    }

    taskENTER_CRITICAL();
        *snapshot = gt_app_bms_snapshot;
    taskEXIT_CRITICAL();
}
