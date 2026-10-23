#include "cmsis_os.h"
#include "task.h"

#include "App/app_balance_manage.h"
#include "App/app_current_calib.h"
#include "HSI_afe/hsi_afe.h"
#include "Task/afe_task.h"
#include "Task/app_freertos.h"

/* 10ms AFE service loop; each iteration advances sampling, calibration, balance and power state. */
void AFE_Task(void const *argument)
{
    (void)argument;

    for (;;)
    {
        if (hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
        {
            if (gt_hsi_afe.shutdown_requested == 0U)
            {
                hsi_afe_task();

                app_current_calib_task();
                app_balance_task();
            }

            hsi_afe_release();
            app_freertos_task_heartbeat(APP_FREERTOS_TASK_AFE);

        }
        (void)osDelay(10U);
    }
}
