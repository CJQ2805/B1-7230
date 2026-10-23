#include "cmsis_os.h"
#include "task.h"

#include "App/app_can.h"
#include "App/app_param.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#include "Protocol/UDS/UDSBase/uds_port.h"
#include "Task/comm_task.h"
#include "Task/app_freertos.h"

/*
 * CAN通信任务主循环：先排空底层CAN接收环形队列，再推进发送协议状态机；
 * 最后的UART IAP调用与CAN无关，保留在同一通信任务中统一调度。
 */
void Comm_Task(void const *argument)
{
    (void)argument;

    for (;;)
    {
        CAN_ReceiveTask();
        CAN_TransmitTask();
#if (APP_PARAM_UDS_ENABLE == 1U)
        /* UDS内部P2/S3等定时器由通信任务按1ms周期推进。 */
        uds_1ms_task();
#endif
        ducar_iap_protocol_task();

        app_freertos_task_heartbeat(APP_FREERTOS_TASK_COMM);

        (void)osDelay(1U);
    }
}
