#include "App/app_power_manage.h"

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "App/app_bms_snapshot.h"
#include "App/app_h_power_manage.h"
#include "App/app_log_manage.h"
#include "App/app_low_power_manage.h"
#include "App/app_hw_apply.h"
#include "Driver/d_adc.h"
#include "Driver/d_system.h"
#include "Driver/d_timer.h"
#include "HSI_afe/hsi_afe.h"
#include "RTT/SEGGER_RTT.h"
#include "task/bms_task.h"

#define APP_POWER_SHUTDOWN_DELAY_MS       ((uint32_t)10000U)
#define APP_POWER_FINAL_LOG_TIMEOUT_MS    ((uint32_t)2000U)
#define APP_POWER_RESET_FALLBACK_MS       ((uint32_t)3000U)
#define APP_POWER_SOC_RETRY_MS            ((uint32_t)1000U)

typedef enum
{
    APP_POWER_STATE_IDLE = 0U,
    APP_POWER_STATE_WAIT_15S,
    APP_POWER_STATE_WAIT_MOS_OFF,
    APP_POWER_STATE_WAIT_FINAL_LOG,
    APP_POWER_STATE_WAIT_POWER_DOWN,
    APP_POWER_STATE_COMPLETE
} app_power_state_t;

static volatile app_power_state_t s_app_power_state = APP_POWER_STATE_IDLE;
static uint32_t s_app_power_state_start_ms;
static uint8_t s_app_power_initialized;
static volatile uint8_t s_app_power_request_log_pending;
static uint8_t s_app_power_final_log_pending;
static uint8_t s_app_power_soc_attempted;
static uint32_t s_app_power_soc_attempt_ms;

void app_power_manage_init(void)
{
    s_app_power_state_start_ms = 0U;
    s_app_power_state = APP_POWER_STATE_IDLE;
    s_app_power_request_log_pending = 0U;
    s_app_power_final_log_pending = 0U;
    s_app_power_soc_attempted = 0U;
    s_app_power_soc_attempt_ms = 0U;
    s_app_power_initialized = 1U;
    app_low_power_set_shutdown_inhibit(0U);
    app_h_power_manage_set_poweroff_override(0U);
}

app_result_t app_power_manage_request_shutdown(void)
{
    uint8_t accepted = 0U;

    if (s_app_power_initialized == 0U)
    {
        return APP_RESULT_NOT_INIT;
    }

    /* 先禁止STOP，再发布状态，防止请求与低功耗判定交错。 */
    app_low_power_set_shutdown_inhibit(1U);

    taskENTER_CRITICAL();
    if (s_app_power_state == APP_POWER_STATE_IDLE)
    {
        s_app_power_state_start_ms = d_timer_get_ms();
        s_app_power_state = APP_POWER_STATE_WAIT_15S;
        accepted = 1U;
    }
    taskEXIT_CRITICAL();

    if (accepted != 0U)
    {
        SEGGER_RTT_printf(0,
                          "SHUTDOWN request accepted tick=%lu delay=%lums\r\n",
                          (unsigned long)s_app_power_state_start_ms,
                          (unsigned long)APP_POWER_SHUTDOWN_DELAY_MS);
        /* 记录关机请求时的实时状态；存储故障不应阻止关机流程。 */
        s_app_power_request_log_pending =
            app_log_request_snapshot(LOG_TRIGGER_SOFT_SHUTDOWN) ? 0U : 1U;
    }

    return APP_RESULT_OK;
}

static uint8_t app_power_all_mos_are_off(const app_bms_snapshot_t *snapshot)
{
    if ((snapshot == 0) || (snapshot->afe_status_valid == 0U))
    {
        return 0U;
    }

    return ((snapshot->mos.chg_mos_state == 0U) &&
            (snapshot->mos.dsg_mos_state == 0U) &&
            (snapshot->mos.pre_mos_state == 0U)) ? 1U : 0U;
}

void app_power_manage_task(void)
{
    uint32_t now_ms;
    app_bms_snapshot_t snapshot;
    app_log_flush_status_t flush_status;

    if (s_app_power_initialized == 0U)
    {
        return;
    }

    now_ms = d_timer_get_ms();
    if (s_app_power_request_log_pending != 0U)
    {
        if (app_log_request_snapshot(LOG_TRIGGER_SOFT_SHUTDOWN))
        {
            s_app_power_request_log_pending = 0U;
        }
        else if (app_log_get_flush_status() == APP_LOG_FLUSH_FAILED)
        {
            s_app_power_request_log_pending = 0U;
        }
    }

    switch (s_app_power_state)
    {
        case APP_POWER_STATE_IDLE:
        case APP_POWER_STATE_COMPLETE:
            return;

        case APP_POWER_STATE_WAIT_15S:
            if ((uint32_t)(now_ms - s_app_power_state_start_ms) >= APP_POWER_SHUTDOWN_DELAY_MS)
            {
                app_h_power_manage_set_poweroff_override(1U);
                s_app_power_state = APP_POWER_STATE_WAIT_MOS_OFF;
            }
            return;

        case APP_POWER_STATE_WAIT_MOS_OFF:
            app_bms_snapshot_read(&snapshot);
            if (app_power_all_mos_are_off(&snapshot) != 0U)
            {
                /* 第二条关机LOG保存MOS关闭后的最终快照；写入锁忙时也按2秒上限等待。 */
                s_app_power_final_log_pending = 1U;
                s_app_power_soc_attempted = 0U;
                s_app_power_state_start_ms = now_ms;
                s_app_power_state = APP_POWER_STATE_WAIT_FINAL_LOG;
            }
            return;

        case APP_POWER_STATE_WAIT_FINAL_LOG:
            flush_status = app_log_get_flush_status();
            if (s_app_power_final_log_pending != 0U)
            {
                if (app_log_request_snapshot(LOG_TRIGGER_SOFT_SHUTDOWN))
                {
                    s_app_power_final_log_pending = 0U;
                }
                else if (flush_status == APP_LOG_FLUSH_FAILED)
                {
                    s_app_power_final_log_pending = 0U;
                }
            }

            flush_status = app_log_get_flush_status();
            if (((s_app_power_final_log_pending == 0U) &&
                 ((flush_status == APP_LOG_FLUSH_COMPLETE) ||
                  (flush_status == APP_LOG_FLUSH_FAILED))) ||
                ((uint32_t)(now_ms - s_app_power_state_start_ms) >= APP_POWER_FINAL_LOG_TIMEOUT_MS))
            {
                if ((s_app_power_soc_attempted != 0U) &&
                    ((uint32_t)(now_ms - s_app_power_soc_attempt_ms) < APP_POWER_SOC_RETRY_MS))
                {
                    return;
                }
                s_app_power_soc_attempted = 1U;
                s_app_power_soc_attempt_ms = now_ms;
                /* 在BMS任务内保存最新完整状态；失败时保持MOS关闭并重试。 */
                if (!app_bms_soc_flash_save())
                {
                    SEGGER_RTT_printf(0, "SHUTDOWN stage=soc-save failed; retry\r\n");
                    return;
                }
                /* AFE锁暂不可用时留在此状态重试，绝不越过AFE SHUTDOWN尝试切断12V。 */
                uint8_t bAfeShutdownWritten = hsi_afe_shutdown(&gt_hsi_afe) ? 1U : 0U;

                SEGGER_RTT_printf(0,
                                  "SHUTDOWN stage=afe-command written=%u requested=%u\r\n",
                                  (unsigned int)bAfeShutdownWritten,
                                  (unsigned int)gt_hsi_afe.shutdown_requested);
                if (gt_hsi_afe.shutdown_requested == 0U)
                {
                    return;
                }

                d_adc_deinit();
                app_hw_apply_dcdc_enable(0U);
                s_app_power_state_start_ms = d_timer_get_ms();
                s_app_power_state = APP_POWER_STATE_WAIT_POWER_DOWN;
            }
            return;

        case APP_POWER_STATE_WAIT_POWER_DOWN:
            if ((uint32_t)(now_ms - s_app_power_state_start_ms) >= APP_POWER_RESET_FALLBACK_MS)
            {
                if ((s_app_power_soc_attempted != 0U) &&
                    ((uint32_t)(now_ms - s_app_power_soc_attempt_ms) < APP_POWER_SOC_RETRY_MS))
                {
                    return;
                }
                s_app_power_soc_attempted = 1U;
                s_app_power_soc_attempt_ms = now_ms;
                if (!app_bms_soc_flash_save())
                {
                    SEGGER_RTT_printf(0, "SHUTDOWN stage=reset-soc-save failed; retry\r\n");
                    return;
                }
                SEGGER_RTT_printf(0,
                                  "SHUTDOWN fallback=software-reset elapsed=%lums afe_requested=%u\r\n",
                                  (unsigned long)(now_ms - s_app_power_state_start_ms),
                                  (unsigned int)gt_hsi_afe.shutdown_requested);
                d_timer_delay_ms(10U);
                d_system_reset();
                s_app_power_state = APP_POWER_STATE_COMPLETE;
            }
            return;

        default:
            s_app_power_state = APP_POWER_STATE_COMPLETE;
            return;
    }
}
