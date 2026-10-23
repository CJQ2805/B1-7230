#include "cmsis_os.h"
#include "task.h"

#include "App/app_bms_fault.h"
#include "App/app_log_manage.h"
#include "Task/save_log_task.h"
#include "Task/app_freertos.h"

/* 10ms LOG任务：处理实时LOG保存与导出相关后台工作。 */
void SaveLog_Task(void const *argument)
{
    (void)argument;

    for (;;)
    {
        app_bms_fault_storage_task();
        app_log_task();

        app_freertos_task_heartbeat(APP_FREERTOS_TASK_SAVE_LOG);
        (void)osDelay(10U);
    }
}
