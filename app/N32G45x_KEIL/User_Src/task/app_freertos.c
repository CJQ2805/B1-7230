#include "cmsis_os.h"
#include "task.h"

#include "Driver/d_watchdog.h"
#include "Task/afe_task.h"
#include "Task/bms_task.h"
#include "Task/comm_task.h"
#include "Task/data_handle_task.h"
#include "Task/default_task.h"
#include "Task/save_log_task.h"
#include "Task/stack_monitor_task.h"
#include "Task/app_freertos.h"

osThreadId defaultTaskHandle;
osThreadId bmsTaskHandle;
osThreadId afeTaskHandle;
osThreadId commTaskHandle;
osThreadId dataHandleTaskHandle;
osThreadId saveLogTaskHandle;
osThreadId stackMonitorTaskHandle;

volatile app_freertos_monitor_t gt_app_freertos_monitor =
{
    .magic = 0x46524D4FUL,
    .sample_period_ms = APP_STACK_MONITOR_PERIOD_MS,
    .task_count = APP_FREERTOS_TASK_COUNT
};

/* 任务正常节拍和允许的最大相邻心跳间隔，单位ms。 */
static const uint32_t s_app_freertos_expected_period_ms[APP_FREERTOS_TASK_COUNT] =
{
    10U,   /* Default_Task */
    10U,   /* BMS_Task */
    10U,   /* AFE_Task */
    5U,    /* Comm_Task */
    10U,   /* DataHandle_Task */
    10U,   /* SaveLog_Task */
    1000U  /* StackMonitor_Task */
};

static const uint32_t s_app_freertos_deadline_timeout_ms[APP_FREERTOS_TASK_COUNT] =
{
    100U,  /* Default_Task */
    50U,   /* BMS_Task：保护任务超过50ms未完成一轮即视为高风险超时。 */
    50U,   /* AFE_Task：采样和AFE服务超过50ms未完成一轮即视为高风险超时。 */
    100U,  /* Comm_Task */
    50U,   /* DataHandle_Task：快照超过50ms未完成一轮即视为高风险超时。 */
    100U,  /* SaveLog_Task */
    1500U  /* StackMonitor_Task */
};

/* 任务是业务运行的最低完整集合；创建失败时禁止启动不完整的调度器。 */
static void app_freertos_require_task(osThreadId task_handle)
{
    if (task_handle == 0)
    {
        taskDISABLE_INTERRUPTS();
        for (;;)
        {
        }
    }
}

static uint32_t app_freertos_get_task_stack_watermark(osThreadId task_handle)
{
    if (task_handle == 0)
    {
        return 0U;
    }

    return (uint32_t)uxTaskGetStackHighWaterMark((TaskHandle_t)task_handle);
}

void app_freertos_task_heartbeat(app_freertos_task_id_t eTaskId)
{
    volatile app_freertos_task_health_t *ptTaskHealth;
    uint32_t u32CurrentTick;

    if ((uint32_t)eTaskId >= (uint32_t)APP_FREERTOS_TASK_COUNT)
    {
        return;
    }

    ptTaskHealth = &gt_app_freertos_monitor.task_health[eTaskId];
    u32CurrentTick = (uint32_t)xTaskGetTickCount();
    if ((ptTaskHealth->heartbeat != 0U) &&
        ((uint32_t)(u32CurrentTick - ptTaskHealth->last_run_tick) > ptTaskHealth->deadline_timeout_ms))
    {
        ptTaskHealth->deadline_miss_count++;
        ptTaskHealth->last_deadline_miss_tick = u32CurrentTick;
        gt_app_freertos_monitor.task_deadline_miss_flags |= (1UL << (uint32_t)eTaskId);
    }

    ptTaskHealth->heartbeat++;
    ptTaskHealth->last_run_tick = u32CurrentTick;
}

/* osThreadDef stacksz is expressed in bytes; cmsis_os.c converts it to
 * FreeRTOS StackType_t words. */
#define APP_TASK_STACK_SIZE_BYTES(value) ((uint32_t)(value))

/* 除任务栈外至少留8 KiB给TCB、互斥量及后续运行时分配。 */
typedef char app_freertos_heap_budget_check[
    (APP_FREERTOS_TOTAL_STACK_BYTES + (8U * 1024U) <= configTOTAL_HEAP_SIZE) ? 1 : -1];

void app_freertos_init(void)
{
    uint32_t u32TaskIndex;

    for (u32TaskIndex = 0U; u32TaskIndex < (uint32_t)APP_FREERTOS_TASK_COUNT; u32TaskIndex++)
    {
        gt_app_freertos_monitor.task_health[u32TaskIndex].expected_period_ms =
            s_app_freertos_expected_period_ms[u32TaskIndex];
        gt_app_freertos_monitor.task_health[u32TaskIndex].deadline_timeout_ms =
            s_app_freertos_deadline_timeout_ms[u32TaskIndex];
    }

    osThreadDef(defaultTask, app_default_task, osPriorityLow, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_DEFAULT_STACK_BYTES));
    osThreadDef(BMS_Task, BMS_Task, osPriorityHigh, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_BMS_STACK_BYTES));
    osThreadDef(AFE_Task, AFE_Task, osPriorityRealtime, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_AFE_STACK_BYTES));
    osThreadDef(Comm_Task, Comm_Task, osPriorityAboveNormal, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_COMM_STACK_BYTES));
    osThreadDef(DataHandle_Task, DataHandle_Task, osPriorityHigh, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_DATA_HANDLE_STACK_BYTES));
    osThreadDef(SaveLog_Task, SaveLog_Task, osPriorityBelowNormal, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_SAVE_LOG_STACK_BYTES));
    osThreadDef(StackMonitor_Task, StackMonitor_Task, osPriorityBelowNormal, 0, APP_TASK_STACK_SIZE_BYTES(APP_FREERTOS_STACK_MONITOR_STACK_BYTES));

    defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);
    app_freertos_require_task(defaultTaskHandle);
    bmsTaskHandle = osThreadCreate(osThread(BMS_Task), NULL);
    app_freertos_require_task(bmsTaskHandle);
    afeTaskHandle = osThreadCreate(osThread(AFE_Task), NULL);
    app_freertos_require_task(afeTaskHandle);
    commTaskHandle = osThreadCreate(osThread(Comm_Task), NULL);
    app_freertos_require_task(commTaskHandle);
    dataHandleTaskHandle = osThreadCreate(osThread(DataHandle_Task), NULL);
    app_freertos_require_task(dataHandleTaskHandle);
    saveLogTaskHandle = osThreadCreate(osThread(SaveLog_Task), NULL);
    app_freertos_require_task(saveLogTaskHandle);
    stackMonitorTaskHandle = osThreadCreate(osThread(StackMonitor_Task), NULL);
    app_freertos_require_task(stackMonitorTaskHandle);
}

void app_freertos_get_stack_watermarks(app_freertos_stack_watermark_t *watermarks)
{
    if (watermarks == NULL)
    {
        return;
    }

    watermarks->default_task_words = app_freertos_get_task_stack_watermark(defaultTaskHandle);
    watermarks->bms_task_words = app_freertos_get_task_stack_watermark(bmsTaskHandle);
    watermarks->afe_task_words = app_freertos_get_task_stack_watermark(afeTaskHandle);
    watermarks->comm_task_words = app_freertos_get_task_stack_watermark(commTaskHandle);
    watermarks->data_handle_task_words = app_freertos_get_task_stack_watermark(dataHandleTaskHandle);
    watermarks->save_log_task_words = app_freertos_get_task_stack_watermark(saveLogTaskHandle);
}

void vApplicationMallocFailedHook(void)
{
    gt_app_freertos_monitor.malloc_failed_flag = 1U;
    gt_app_freertos_monitor.malloc_failed_count++;
    gt_app_freertos_monitor.last_malloc_failed_tick =
        (uint32_t)xTaskGetTickCount();

    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

static void app_freertos_record_stack_overflow(const char *task_name)
{
    uint32_t u32NameIndex;

    gt_app_freertos_monitor.stack_overflow_flag = 1U;
    gt_app_freertos_monitor.stack_overflow_count++;
    gt_app_freertos_monitor.last_stack_overflow_tick =
        (uint32_t)xTaskGetTickCount();

    for (u32NameIndex = 0U; u32NameIndex < (uint32_t)APP_FREERTOS_MONITOR_TASK_NAME_MAX; u32NameIndex++)
    {
        if ((task_name != NULL) && (task_name[u32NameIndex] != '\0'))
        {
            gt_app_freertos_monitor.last_stack_overflow_task_name[u32NameIndex] = task_name[u32NameIndex];
        }
        else
        {
            gt_app_freertos_monitor.last_stack_overflow_task_name[u32NameIndex] = '\0';
            break;
        }
    }
}

/* 使栈溢出直接停机，避免继续运行造成不可控的业务写入。 */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;

    app_freertos_record_stack_overflow(pcTaskName);

    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}
