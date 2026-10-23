#include "cmsis_os.h"
#include "task.h"

#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "App/app_bms_obc.h"
#include "App/app_input_signals.h"
#include "App/app_mcu_ntc.h"
#include "Board/board_io.h"
#include "Task/data_handle_task.h"
#include "Task/app_freertos.h"

/* 10ms real-time data loop; snapshot and MCU NTC are refreshed before BMS consumes them. */
void DataHandle_Task(void const *argument)
{
    uint16_t u16LedPeriodCount = 0U;

    (void)argument;

    for (;;)
    {
        app_mcu_ntc_task();
        app_input_signals_update();
        app_bms_obc_task(gt_app_input_signals.mcu_detc);
        app_bms_snapshot_update();
        app_bms_status_task();

        u16LedPeriodCount++;
        if (u16LedPeriodCount >= 50U)
        {
            u16LedPeriodCount = 0U;
            board_io_led_toggle();
        }

        app_freertos_task_heartbeat(APP_FREERTOS_TASK_DATA_HANDLE);

        (void)osDelay(10U);
    }
}
