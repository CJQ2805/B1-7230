#ifndef APP_FREERTOS_H
#define APP_FREERTOS_H

#include <stdint.h>

#define APP_FREERTOS_MONITOR_TASK_NAME_MAX      (16U)

/* CMSIS osThreadDef 的 stacksize 单位为字节，统一在这里分配和核对。 */
#define APP_FREERTOS_DEFAULT_STACK_BYTES       (1024U)
#define APP_FREERTOS_BMS_STACK_BYTES           (4096U)
#define APP_FREERTOS_AFE_STACK_BYTES           (2048U)
#define APP_FREERTOS_COMM_STACK_BYTES          (6144U)
#define APP_FREERTOS_DATA_HANDLE_STACK_BYTES   (2048U)
#define APP_FREERTOS_SAVE_LOG_STACK_BYTES      (4096U)
#define APP_FREERTOS_STACK_MONITOR_STACK_BYTES (1024U)
#define APP_FREERTOS_TOTAL_STACK_BYTES         \
    (APP_FREERTOS_DEFAULT_STACK_BYTES + APP_FREERTOS_BMS_STACK_BYTES + \
     APP_FREERTOS_AFE_STACK_BYTES + APP_FREERTOS_COMM_STACK_BYTES + \
     APP_FREERTOS_DATA_HANDLE_STACK_BYTES + APP_FREERTOS_SAVE_LOG_STACK_BYTES + \
     APP_FREERTOS_STACK_MONITOR_STACK_BYTES)

#define APP_FREERTOS_WATCHDOG_BLOCK_TASK_STALL  (1UL << 0)
#define APP_FREERTOS_WATCHDOG_BLOCK_MALLOC      (1UL << 1)
#define APP_FREERTOS_WATCHDOG_BLOCK_STACK       (1UL << 2)
#define APP_FREERTOS_WATCHDOG_BLOCK_NOT_RUNNING (1UL << 3)
#define APP_FREERTOS_WATCHDOG_BLOCK_DEADLINE    (1UL << 4)

typedef enum
{
    APP_FREERTOS_TASK_DEFAULT = 0U,
    APP_FREERTOS_TASK_BMS,
    APP_FREERTOS_TASK_AFE,
    APP_FREERTOS_TASK_COMM,
    APP_FREERTOS_TASK_DATA_HANDLE,
    APP_FREERTOS_TASK_SAVE_LOG,
    APP_FREERTOS_TASK_STACK_MONITOR,
    APP_FREERTOS_TASK_COUNT
} app_freertos_task_id_t;

typedef struct
{
    uint32_t heartbeat;             /* 任务心跳计数；任务每完成一轮业务处理加1。 */
    uint32_t last_run_tick;         /* 最近一次心跳发生时的FreeRTOS Tick值。 */
    uint32_t expected_period_ms;    /* 任务设计运行周期，单位：毫秒。 */
    uint32_t deadline_timeout_ms;   /* 允许相邻两次心跳的最大间隔，单位：毫秒。 */
    uint32_t deadline_miss_count;   /* 任务累计超过deadline的次数。 */
    uint32_t last_deadline_miss_tick; /* 最近一次deadline超时时的FreeRTOS Tick值。 */
} app_freertos_task_health_t;

/* uxTaskGetStackHighWaterMark() returns the minimum remaining stack in
 * StackType_t words for each task. */
typedef struct
{
    uint32_t magic;             /* 监控结构体识别字；用于确认读取到的是有效监控数据。 */
    uint32_t sample_count;      /* 监控任务累计完成的采样次数。 */
    uint32_t last_sample_tick;  /* 最近一次采样发生时的FreeRTOS Tick值。 */
    uint32_t sample_period_ms;  /* 监控任务设计采样周期，单位：毫秒。 */
    uint32_t task_count;        /* 当前纳入心跳和健康检查的任务数量。 */
    uint32_t task_stall_flags;  /* 任务停滞位图；第N位对应app_freertos_task_id_t的第N个任务。 */
    uint32_t task_stall_event_count; /* 检测到任务停滞的累计采样事件次数。 */
    uint32_t task_deadline_miss_flags; /* 任务deadline超时位图；第N位对应第N个任务。 */
    app_freertos_task_health_t task_health[APP_FREERTOS_TASK_COUNT]; /* 各任务的心跳和deadline统计。 */
    uint32_t free_heap_bytes;    /* 当前FreeRTOS可分配堆空间，单位：字节。 */
    uint32_t minimum_ever_free_heap_bytes; /* 系统运行以来的最小剩余堆空间，单位：字节。 */
    uint32_t malloc_failed_flag; /* 内存申请失败标志；1表示曾发生申请失败。 */
    uint32_t malloc_failed_count; /* 内存申请失败累计次数。 */
    uint32_t last_malloc_failed_tick; /* 最近一次内存申请失败时的FreeRTOS Tick值。 */
    uint32_t stack_overflow_flag; /* 栈溢出标志；1表示检测到栈溢出。 */
    uint32_t stack_overflow_count; /* 栈溢出累计次数。 */
    uint32_t last_stack_overflow_tick; /* 最近一次栈溢出发生时的FreeRTOS Tick值。 */
    char last_stack_overflow_task_name[APP_FREERTOS_MONITOR_TASK_NAME_MAX]; /* 最近发生栈溢出的任务名。 */
    uint32_t default_task_stack_free_bytes; /* Default_Task剩余栈水位，单位：字节。 */
    uint32_t bms_task_stack_free_bytes; /* BMS_Task剩余栈水位，单位：字节。 */
    uint32_t afe_task_stack_free_bytes; /* AFE_Task剩余栈水位，单位：字节。 */
    uint32_t comm_task_stack_free_bytes; /* Comm_Task剩余栈水位，单位：字节。 */
    uint32_t data_handle_task_stack_free_bytes; /* DataHandle_Task剩余栈水位，单位：字节。 */
    uint32_t save_log_task_stack_free_bytes; /* SaveLog_Task剩余栈水位，单位：字节。 */
    uint32_t stack_monitor_task_stack_free_bytes; /* StackMonitor_Task剩余栈水位，单位：字节。 */
    uint32_t idle_task_stack_free_bytes; /* FreeRTOS Idle任务剩余栈水位，单位：字节。 */
    uint32_t stack_low_margin_flags; /* 历史低余量位图；bit0..6对应业务任务，bit7对应Idle。 */
    uint32_t stack_low_margin_event_count; /* 新任务出现低余量时的累计采样次数。 */
    uint32_t watchdog_running;   /* 看门狗运行状态；1表示已运行，0表示未运行。 */
    uint32_t watchdog_feed_count; /* 看门狗当前喂狗计数，单位：毫秒计数值。 */
    uint32_t watchdog_feed_allowed; /* 本轮是否允许喂狗；1允许，0禁止。 */
    uint32_t watchdog_feed_blocked_flags; /* 禁止喂狗原因位图。 */
    uint32_t watchdog_feed_stall_flag; /* 喂狗计数停滞标志；1表示允许喂狗但计数未变化。 */
    uint32_t watchdog_feed_stall_event_count; /* 喂狗计数停滞事件累计次数。 */
} app_freertos_monitor_t;

typedef struct
{
    uint32_t default_task_words;       /* Default_Task剩余栈水位，单位：StackType_t字。 */
    uint32_t bms_task_words;           /* BMS_Task剩余栈水位，单位：StackType_t字。 */
    uint32_t afe_task_words;           /* AFE_Task剩余栈水位，单位：StackType_t字。 */
    uint32_t comm_task_words;          /* Comm_Task剩余栈水位，单位：StackType_t字。 */
    uint32_t data_handle_task_words;   /* DataHandle_Task剩余栈水位，单位：StackType_t字。 */
    uint32_t save_log_task_words;      /* SaveLog_Task剩余栈水位，单位：StackType_t字。 */
} app_freertos_stack_watermark_t;

/* Non-static and volatile so the debugger can watch the complete monitor
 * state directly while the system is running. */
extern volatile app_freertos_monitor_t gt_app_freertos_monitor;

/* Initialize FreeRTOS resources and the task-centric application tasks. */
void app_freertos_init(void);

/* Mark one task iteration as completed for the system health monitor. */
void app_freertos_task_heartbeat(app_freertos_task_id_t task_id);

/* Query task stack watermarks from task context after app_freertos_init(). */
void app_freertos_get_stack_watermarks(app_freertos_stack_watermark_t *watermarks);

#endif /* APP_FREERTOS_H */
