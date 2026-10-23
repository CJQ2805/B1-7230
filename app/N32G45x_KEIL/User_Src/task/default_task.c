#include "cmsis_os.h"
#include "task.h"

#include "Task/default_task.h"
#include "Task/app_freertos.h"

/* The default task has no business work in phase 2. */
void app_default_task(void const *argument)
{
    (void)argument;

    for (;;)
    {
        app_freertos_task_heartbeat(APP_FREERTOS_TASK_DEFAULT);

        (void)osDelay(10U);
    }
}
