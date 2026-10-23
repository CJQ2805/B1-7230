#ifndef __APP_POWER_MANAGE_H__
#define __APP_POWER_MANAGE_H__

#include "Self_Driver/sd_result.h"

/* 初始化软关机管理器；应在创建业务任务前调用。 */
void app_power_manage_init(void);

/* 受理软关机请求；多次请求幂等，不会重启15秒等待。 */
app_result_t app_power_manage_request_shutdown(void);

/* 由BMS 10ms任务调用，推进非阻塞软关机状态机。 */
void app_power_manage_task(void);

#endif
