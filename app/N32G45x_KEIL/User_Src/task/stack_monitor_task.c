#include "cmsis_os.h"
#include "task.h"

#include "Driver/d_watchdog.h"
#include "Task/stack_monitor_task.h"
#include "Task/app_freertos.h"
#include "SEGGER_RTT.h"

/* 水位低于分配量的25%或512字节时记录预警，避免只在真正溢出后才发现。 */
static uint32_t app_stack_monitor_low_margin_bit(uint32_t free_bytes,
                                                 uint32_t stack_bytes,
                                                 app_freertos_task_id_t task_id)
{
    uint32_t limit_bytes = stack_bytes / 4U;

    if (limit_bytes < 512U)
    {
        limit_bytes = 512U;
    }

    return (free_bytes <= limit_bytes) ? (1UL << (uint32_t)task_id) : 0U;
}

static uint32_t app_stack_monitor_words_to_bytes(uint32_t words)
{
    return words * (uint32_t)sizeof(StackType_t);
}

static void app_stack_monitor_update_task_health(uint32_t *au32PreviousHeartbeats,
                                                  uint8_t bBaselineReady)
{
    uint32_t u32TaskIndex;
    uint32_t u32TaskStallFlags = 0U;

    for (u32TaskIndex = 0U; u32TaskIndex < (uint32_t)APP_FREERTOS_TASK_COUNT; u32TaskIndex++)
    {
        uint32_t u32CurrentHeartbeat =
            gt_app_freertos_monitor.task_health[u32TaskIndex].heartbeat;

        if ((bBaselineReady != 0U) &&
            (u32CurrentHeartbeat == au32PreviousHeartbeats[u32TaskIndex]))
        {
            u32TaskStallFlags |= (1UL << u32TaskIndex);
        }

        au32PreviousHeartbeats[u32TaskIndex] = u32CurrentHeartbeat;
    }

    gt_app_freertos_monitor.task_stall_flags = u32TaskStallFlags;
    if (u32TaskStallFlags != 0U)
    {
        gt_app_freertos_monitor.task_stall_event_count++;
    }
}

static uint32_t app_stack_monitor_get_watchdog_block_flags(void)
{
    uint32_t u32BlockedWatchdogFlags = 0U;

    if (gt_app_freertos_monitor.task_stall_flags != 0U)
    {
        u32BlockedWatchdogFlags |= APP_FREERTOS_WATCHDOG_BLOCK_TASK_STALL;
    }

    if ((gt_app_freertos_monitor.task_deadline_miss_flags &
         ((1UL << APP_FREERTOS_TASK_AFE) |
          (1UL << APP_FREERTOS_TASK_BMS) |
          (1UL << APP_FREERTOS_TASK_DATA_HANDLE))) != 0U)
    {
        u32BlockedWatchdogFlags |= APP_FREERTOS_WATCHDOG_BLOCK_DEADLINE;
    }

    if (gt_app_freertos_monitor.malloc_failed_flag != 0U)
    {
        u32BlockedWatchdogFlags |= APP_FREERTOS_WATCHDOG_BLOCK_MALLOC;
    }

    if (gt_app_freertos_monitor.stack_overflow_flag != 0U)
    {
        u32BlockedWatchdogFlags |= APP_FREERTOS_WATCHDOG_BLOCK_STACK;
    }

    if (gt_app_freertos_monitor.watchdog_running == 0U)
    {
        u32BlockedWatchdogFlags |= APP_FREERTOS_WATCHDOG_BLOCK_NOT_RUNNING;
    }

    return u32BlockedWatchdogFlags;
}

void StackMonitor_Task(void const *argument)
{
    app_freertos_stack_watermark_t watermarks;
    uint32_t au32PreviousHeartbeats[APP_FREERTOS_TASK_COUNT] = {0U};
    uint32_t u32PreviousWatchdogFeedCount = 0U;
    uint8_t bPreviousWatchdogFeedAllowed = 0U;
    uint8_t bHealthBaselineReady = 0U;
    uint32_t u32LowStackFlags;
    uint32_t u32NewLowStackFlags;

    (void)argument;

    for (;;)
    {
        app_freertos_task_heartbeat(APP_FREERTOS_TASK_STACK_MONITOR);
        app_stack_monitor_update_task_health(au32PreviousHeartbeats,
                                             bHealthBaselineReady);

        app_freertos_get_stack_watermarks(&watermarks);

        gt_app_freertos_monitor.default_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.default_task_words);
        gt_app_freertos_monitor.bms_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.bms_task_words);
        gt_app_freertos_monitor.afe_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.afe_task_words);
        gt_app_freertos_monitor.comm_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.comm_task_words);
        gt_app_freertos_monitor.data_handle_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.data_handle_task_words);
        gt_app_freertos_monitor.save_log_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(watermarks.save_log_task_words);
        gt_app_freertos_monitor.stack_monitor_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(
                (uint32_t)uxTaskGetStackHighWaterMark(NULL));
        gt_app_freertos_monitor.idle_task_stack_free_bytes =
            app_stack_monitor_words_to_bytes(
                (uint32_t)uxTaskGetStackHighWaterMark(xTaskGetIdleTaskHandle()));

        u32LowStackFlags =
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.default_task_stack_free_bytes,
                                             APP_FREERTOS_DEFAULT_STACK_BYTES, APP_FREERTOS_TASK_DEFAULT) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.bms_task_stack_free_bytes,
                                             APP_FREERTOS_BMS_STACK_BYTES, APP_FREERTOS_TASK_BMS) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.afe_task_stack_free_bytes,
                                             APP_FREERTOS_AFE_STACK_BYTES, APP_FREERTOS_TASK_AFE) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.comm_task_stack_free_bytes,
                                             APP_FREERTOS_COMM_STACK_BYTES, APP_FREERTOS_TASK_COMM) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.data_handle_task_stack_free_bytes,
                                             APP_FREERTOS_DATA_HANDLE_STACK_BYTES, APP_FREERTOS_TASK_DATA_HANDLE) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.save_log_task_stack_free_bytes,
                                             APP_FREERTOS_SAVE_LOG_STACK_BYTES, APP_FREERTOS_TASK_SAVE_LOG) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.stack_monitor_task_stack_free_bytes,
                                             APP_FREERTOS_STACK_MONITOR_STACK_BYTES, APP_FREERTOS_TASK_STACK_MONITOR) |
            app_stack_monitor_low_margin_bit(gt_app_freertos_monitor.idle_task_stack_free_bytes,
                                             (uint32_t)configMINIMAL_STACK_SIZE * (uint32_t)sizeof(StackType_t),
                                             APP_FREERTOS_TASK_COUNT);
        u32NewLowStackFlags = u32LowStackFlags & ~gt_app_freertos_monitor.stack_low_margin_flags;
        if (u32NewLowStackFlags != 0U)
        {
            gt_app_freertos_monitor.stack_low_margin_flags |= u32NewLowStackFlags;
            gt_app_freertos_monitor.stack_low_margin_event_count++;
            if (gt_app_freertos_monitor.stack_monitor_task_stack_free_bytes >= 768U)
            {
                SEGGER_RTT_printf(0, "STACK low margin task_mask=0x%02lX\r\n",
                                  (unsigned long)u32NewLowStackFlags);
            }
        }

        gt_app_freertos_monitor.free_heap_bytes =
            (uint32_t)xPortGetFreeHeapSize();
        gt_app_freertos_monitor.minimum_ever_free_heap_bytes =
            (uint32_t)xPortGetMinimumEverFreeHeapSize();
        gt_app_freertos_monitor.last_sample_tick =
            (uint32_t)xTaskGetTickCount();
        gt_app_freertos_monitor.watchdog_running =
            d_watchdog_is_running() ? 1U : 0U;
        gt_app_freertos_monitor.watchdog_feed_count =
            d_watchdog_get_counter_ms();

        /* The monitor task owns the normal feed; a failed health check lets IWDG reset. */
        gt_app_freertos_monitor.watchdog_feed_blocked_flags =
            app_stack_monitor_get_watchdog_block_flags();
        gt_app_freertos_monitor.watchdog_feed_allowed =
            (gt_app_freertos_monitor.watchdog_feed_blocked_flags == 0U) ? 1U : 0U;

        gt_app_freertos_monitor.watchdog_feed_stall_flag =
            ((bHealthBaselineReady != 0U) &&
             (bPreviousWatchdogFeedAllowed != 0U) &&
             (gt_app_freertos_monitor.watchdog_running != 0U) &&
             (gt_app_freertos_monitor.watchdog_feed_count == u32PreviousWatchdogFeedCount)) ?
                1U : 0U;
        if (gt_app_freertos_monitor.watchdog_feed_stall_flag != 0U)
        {
            gt_app_freertos_monitor.watchdog_feed_stall_event_count++;
        }
        u32PreviousWatchdogFeedCount = gt_app_freertos_monitor.watchdog_feed_count;
        bPreviousWatchdogFeedAllowed =
            (uint8_t)gt_app_freertos_monitor.watchdog_feed_allowed;

        if (gt_app_freertos_monitor.watchdog_feed_allowed != 0U)
        {
            d_watchdog_feed();
        }

        gt_app_freertos_monitor.sample_count++;
        bHealthBaselineReady = 1U;

        (void)osDelay(APP_STACK_MONITOR_PERIOD_MS);
    }
}
