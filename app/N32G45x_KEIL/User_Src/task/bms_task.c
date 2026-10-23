#include <stdint.h>
#include <string.h>

#include "cmsis_os.h"
#include "task.h"

#include "App/app_bms_fault.h"
#include "App/app_bms_snapshot.h"
#include "App/app_charge_time.h"
#include "App/app_h_power_manage.h"
#include "App/app_long_time_undervoltage_shutdown.h"
#include "App/app_led.h"
#include "App/app_low_power_manage.h"
#include "App/app_power_manage.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "App/app_flash_map.h"
#include "Board/board_io.h"
#include "app_soc.h"
#include "Driver/d_flash.h"
#include "Driver/d_timer.h"
#include "RTT/SEGGER_RTT.h"
#include "Task/bms_task.h"
#include "Task/app_freertos.h"

#pragma pack(push, 1)
/* Page1只保存“SC”两字节标识和SOC算法数据本身，不另加版本、长度或完成标志。 */
typedef struct
{
    uint16_t tag;
    uint8_t soc_data[sizeof(struct _app_soc_t)];
} app_bms_soc_flash_record_t;
#pragma pack(pop)

typedef char app_bms_soc_flash_record_fits_page[
    (sizeof(app_bms_soc_flash_record_t) <= APP_FLASH_MAP_FLASH_PAGE_SIZE) ? 1 : -1];

static unsigned char s_app_soc_force_ocv_flag = 1U;
static volatile uint8_t s_app_soc_ocv_save_status = APP_BMS_SOC_OCV_IDLE;
static volatile uint8_t s_app_soc_write_status = APP_BMS_SOC_WRITE_IDLE;
static volatile uint8_t s_app_soc_write_target;
static uint8_t s_bms_shutdown_trigger_logged;

/* 读取固定SOC页；“SC”标识或关键SOC数值无效时使用正常上电查表流程。 */
static bool app_bms_soc_flash_restore(void)
{
    const app_bms_soc_flash_record_t *record =
        (const app_bms_soc_flash_record_t *)APP_FLASH_MAP_DB_SOC_ADDR;
    struct _app_soc_t restored_soc;

    if (record->tag != APP_FLASH_TAG_SOC)
    {
        return false;
    }

    memcpy(&restored_soc, record->soc_data, sizeof(restored_soc));
    if ((restored_soc.s32battery_full_cap <= 0L) ||
        (restored_soc.s32battery_full_cap > 1000000L) ||
        (restored_soc.s32battery_full_cap < MIN_FULLCHG_CAP) ||
        (restored_soc.s32battery_full_cap > MAX_FULLCHG_CAP) ||
        (restored_soc.s32remain_cap < 0L) ||
        (restored_soc.s32remain_cap > restored_soc.s32battery_full_cap) ||
        (restored_soc.tsoc_capacity_data.s32real_cap_val < 0L) ||
        (restored_soc.tsoc_capacity_data.s32real_cap_val > restored_soc.s32battery_full_cap) ||
        (restored_soc.tsoc_capacity_data.s32display_cap_val < 0L) ||
        (restored_soc.tsoc_capacity_data.s32display_cap_val > restored_soc.s32battery_full_cap) ||
        (restored_soc.u8display_soc > 100U) ||
        (restored_soc.u8display_soh > 100U) ||
        (restored_soc.tsoc_fcc_update.u8SocInit > 100U))
    {
        return false;
    }

    memcpy(&gt_app_soc, &restored_soc, sizeof(gt_app_soc));
    /* 计时基准不跨复位恢复，避免沿用掉电前的延时计数。 */
    gt_app_soc.u32tim_250ms_cnt = 0UL;
    return true;
}

/* 以单页固定地址保存SOC快照；写后回读比对，数据未变化时跳过擦写。 */
static bool app_bms_soc_flash_save(void)
{
    app_bms_soc_flash_record_t record;
    const app_bms_soc_flash_record_t *flash_record =
        (const app_bms_soc_flash_record_t *)APP_FLASH_MAP_DB_SOC_ADDR;
    struct _app_soc_t saved_soc;
    bool result = false;

    if ((gt_app_soc.s32battery_full_cap <= 0L) ||
        (gt_app_soc.s32battery_full_cap > 1000000L) ||
        (gt_app_soc.s32remain_cap < 0L) ||
        (gt_app_soc.s32remain_cap > gt_app_soc.s32battery_full_cap) ||
        (gt_app_soc.tsoc_capacity_data.s32real_cap_val < 0L) ||
        (gt_app_soc.tsoc_capacity_data.s32real_cap_val > gt_app_soc.s32battery_full_cap) ||
        (gt_app_soc.tsoc_capacity_data.s32display_cap_val < 0L) ||
        (gt_app_soc.tsoc_capacity_data.s32display_cap_val > gt_app_soc.s32battery_full_cap) ||
        (gt_app_soc.u8display_soc > 100U))
    {
        return false;
    }

    memset(&record, 0xFF, sizeof(record));
    record.tag = APP_FLASH_TAG_SOC;
    memcpy(&saved_soc, &gt_app_soc, sizeof(saved_soc));
    saved_soc.u32tim_250ms_cnt = 0UL;
    memcpy(record.soc_data, &saved_soc, sizeof(saved_soc));

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return false;
    }

    if (memcmp(flash_record, &record, sizeof(record)) == 0)
    {
        result = true;
    }
    else if (d_flash_erase_page(APP_FLASH_MAP_DB_SOC_ADDR) &&
             d_flash_write(APP_FLASH_MAP_DB_SOC_ADDR,
                           (const uint8_t *)&record,
                           (uint32_t)sizeof(record)) &&
             (memcmp(flash_record, &record, sizeof(record)) == 0))
    {
        result = true;
    }

    d_flash_release();
    return result;
}

bool app_bms_force_soc_ocv(void)
{
    bool accepted = false;

    taskENTER_CRITICAL();
    if ((s_app_soc_ocv_save_status == APP_BMS_SOC_OCV_IDLE) &&
        (s_app_soc_write_status == APP_BMS_SOC_WRITE_IDLE) &&
        (s_app_soc_force_ocv_flag == 0U))
    {
        s_app_soc_ocv_save_status = APP_BMS_SOC_OCV_PENDING;
        s_app_soc_force_ocv_flag = 1U;
        accepted = true;
    }
    taskEXIT_CRITICAL();

    return accepted;
}

uint8_t app_bms_soc_ocv_save_status(void)
{
    return s_app_soc_ocv_save_status;
}

void app_bms_soc_ocv_complete(void)
{
    taskENTER_CRITICAL();
    s_app_soc_ocv_save_status = APP_BMS_SOC_OCV_IDLE;
    taskEXIT_CRITICAL();
}

bool app_bms_soc_write_request(uint8_t requested_soc)
{
    bool accepted = false;

    if (requested_soc > 100U)
    {
        return false;
    }

    taskENTER_CRITICAL();
    /* E200与强制OCV重算互斥，避免两个命令同时改写SOC状态。 */
    if ((s_app_soc_write_status == APP_BMS_SOC_WRITE_IDLE) &&
        (s_app_soc_ocv_save_status == APP_BMS_SOC_OCV_IDLE) &&
        (s_app_soc_force_ocv_flag == 0U))
    {
        s_app_soc_write_target = requested_soc;
        s_app_soc_write_status = APP_BMS_SOC_WRITE_PENDING;
        accepted = true;
    }
    taskEXIT_CRITICAL();

    return accepted;
}

uint8_t app_bms_soc_write_status(void)
{
    uint8_t status;

    taskENTER_CRITICAL();
    status = s_app_soc_write_status;
    taskEXIT_CRITICAL();
    return status;
}

void app_bms_soc_write_complete(void)
{
    taskENTER_CRITICAL();
    if ((s_app_soc_write_status == APP_BMS_SOC_WRITE_SUCCESS) ||
        (s_app_soc_write_status == APP_BMS_SOC_WRITE_FAILED))
    {
        s_app_soc_write_status = APP_BMS_SOC_WRITE_IDLE;
    }
    taskEXIT_CRITICAL();
}

static void app_bms_process_soc_write(void)
{
    app_bms_snapshot_t snapshot;
    struct _app_soc_t previous_soc;
    uint8_t requested_soc;
    uint8_t should_validate = 0U;
    uint8_t result;

    taskENTER_CRITICAL();
    if (s_app_soc_write_status == APP_BMS_SOC_WRITE_PENDING)
    {
        requested_soc = s_app_soc_write_target;
        s_app_soc_write_status = APP_BMS_SOC_WRITE_RUNNING;
        should_validate = 1U;
    }
    else
    {
        taskEXIT_CRITICAL();
        return;
    }
    taskEXIT_CRITICAL();

    if (should_validate != 0U)
    {
        app_bms_snapshot_read(&snapshot);
        result = APP_BMS_SOC_WRITE_FAILED;
        if ((snapshot.data_ready != 0U) &&
            (s_app_soc_force_ocv_flag == 0U) &&
            (app_soc_can_write_matches_table(requested_soc,
                                             snapshot.max_cell_mv,
                                             g_app_param_soc_cell_ocv_table) != 0U))
        {
            previous_soc = gt_app_soc;
            if ((app_soc_set_requested_percent(requested_soc) != 0U) &&
                app_bms_soc_flash_save())
            {
                result = APP_BMS_SOC_WRITE_SUCCESS;
            }
            else
            {
                /* Flash写入失败时回滚RAM状态，避免运行SOC与持久化结果不一致。 */
                gt_app_soc = previous_soc;
            }
        }

        taskENTER_CRITICAL();
        if (s_app_soc_write_status == APP_BMS_SOC_WRITE_RUNNING)
        {
            s_app_soc_write_status = result;
        }
        taskEXIT_CRITICAL();
    }
}

static void app_bms_run_soc(void)
{
    app_bms_snapshot_t snapshot;
    uint32_t previous_ocv_update_count;
    uint8_t u8SocUcvFlag;
    uint8_t u8SocOcvFlag;
    uint8_t u8SocTickFlag = 1U;

    /* E200从排队到Flash提交并完成CAN应答期间冻结SOC积分，确保Flash候选值与应用值一致。 */
    if (app_bms_soc_write_status() != APP_BMS_SOC_WRITE_IDLE)
    {
        return;
    }

    app_bms_snapshot_read(&snapshot);
    /* 完整电芯快照且最高单体达到SOC_Init的有效电压要求后才运行算法。 */
    if ((snapshot.data_ready == 0U) || (snapshot.max_cell_mv < 2000U))
    {
        return;
    }

    u8SocUcvFlag = (uint8_t)((snapshot.afe_fault.cell_uv != 0U) ||
        (gt_app_bms_fault.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 != 0U) ||
        (gt_app_bms_fault.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 != 0U));
    u8SocOcvFlag = (uint8_t)((snapshot.afe_fault.cell_ov != 0U) ||
        (gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov != 0U));

    previous_ocv_update_count = app_soc_ocv_update_count();

    (void)app_soc_data_handle(snapshot.max_cell_mv,
                              snapshot.min_cell_mv,
                              g_app_param_soc_cell_ocv_table,
                              (long)snapshot.current_ma,
                              &u8SocTickFlag,
                              (int)snapshot.min_temp_c,
                              (int)snapshot.max_temp_c,
                              u8SocUcvFlag,
                              u8SocOcvFlag,
                              &s_app_soc_force_ocv_flag,
                              0U);

    /* 仅在SOC发生OCV查表校正时落盘，避免固定单页随普通库仑积分频繁擦写。 */
    if (previous_ocv_update_count != app_soc_ocv_update_count())
    {
        bool saved = app_bms_soc_flash_save();
        if ((s_app_soc_ocv_save_status == APP_BMS_SOC_OCV_PENDING) &&
            (s_app_soc_force_ocv_flag == 0U))
        {
            s_app_soc_ocv_save_status = saved ?
                APP_BMS_SOC_OCV_SAVED : APP_BMS_SOC_OCV_SAVE_FAILED;
        }
    }
}

/* 10ms BMS protection/MOS loop; SOC expiry comes from the hardware 1ms timer counter. */
void BMS_Task(void const *argument)
{
    u32TimDly_t soc_delay;
    struct _app_bms_fault_t fault_snapshot;

    (void)argument;
    /* 先恢复固定Flash快照；无有效快照时由SOC_Init在首个有效AFE样本上建立默认状态。 */
    if (app_bms_soc_flash_restore())
    {
        /* 有效快照优先于上电强制查表，后续静置OCV校正仍可更新SOC。 */
        s_app_soc_force_ocv_flag = 0U;
    }
    soc_delay = d_timer_reset_count();

    for (;;)
    {
        uint8_t bCuv2Active;
        uint8_t bPfActive;
        uint8_t bCellDiffActive;

        app_bms_fault_task(0U);
        app_bms_fault_snapshot_read(&fault_snapshot);
        bCuv2Active = (fault_snapshot.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 != 0U) ? 1U : 0U;
        bPfActive = (fault_snapshot.tbms_fault_stat.u8PF_fault_flag != 0U) ? 1U : 0U;
        bCellDiffActive = ((fault_snapshot.tbms_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault != 0U) ||
                           (fault_snapshot.tbms_fault_flag.ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 != 0U)) ? 1U : 0U;
        if ((bCuv2Active != 0U) || (bPfActive != 0U))
        {
            /* CUV2立即关机；PF（包含深度欠压）也立即关机。 */
            if (s_bms_shutdown_trigger_logged == 0U)
            {
                app_bms_snapshot_t shutdown_snapshot;

                app_bms_snapshot_read(&shutdown_snapshot);
                SEGGER_RTT_printf(0,
                                  "SHUTDOWN trigger=protection CUV2=%u PF=%u VDIF=%u PFmask=0x%08lX cells[min=%u max=%u gap=%u ready=%u]\r\n",
                                  (unsigned int)bCuv2Active,
                                  (unsigned int)bPfActive,
                                  (unsigned int)bCellDiffActive,
                                  (unsigned long)app_bms_fault_get_pf_mask(),
                                  (unsigned int)shutdown_snapshot.min_cell_mv,
                                  (unsigned int)shutdown_snapshot.max_cell_mv,
                                  (unsigned int)shutdown_snapshot.cell_gap_mv,
                                  (unsigned int)shutdown_snapshot.data_ready);
                s_bms_shutdown_trigger_logged = 1U;
            }
            (void)app_power_manage_request_shutdown();
        }
        else
        {
            s_bms_shutdown_trigger_logged = 0U;
        }
        app_long_time_undervoltage_shutdown_task();
        app_power_manage_task();
        app_h_power_manage_task();

        if (d_timer_delay_check(true, &soc_delay, TMR0_DELAY_MS(250U)) == Ok)
        {
            app_bms_run_soc();
        }
        /* CAN E200只投递主机请求，查表校验、Flash保存和应答状态由BMS任务串行处理。 */
        app_bms_process_soc_write();
        app_bms_snapshot_update_soc();
        app_charge_time_task();
        app_led_task();

        /* 保护、MOS和SOC本轮处理完成后，再由BMS任务统一评估是否进入低功耗。 */
        app_low_power_task(10U);
        app_freertos_task_heartbeat(APP_FREERTOS_TASK_BMS);

        (void)osDelay(10U);

    }
}
