#ifndef APP_BMS_TASK_H
#define APP_BMS_TASK_H

#include <stdint.h>
#include <stdbool.h>

#include "cmsis_os.h"

#define APP_BMS_SOC_WRITE_IDLE       ((uint8_t)0U)
#define APP_BMS_SOC_WRITE_PENDING   ((uint8_t)1U)
#define APP_BMS_SOC_WRITE_RUNNING   ((uint8_t)2U)
#define APP_BMS_SOC_WRITE_SUCCESS   ((uint8_t)3U)
#define APP_BMS_SOC_WRITE_FAILED    ((uint8_t)4U)

#define APP_BMS_SOC_OCV_IDLE         ((uint8_t)0U)
#define APP_BMS_SOC_OCV_PENDING      ((uint8_t)1U)
#define APP_BMS_SOC_OCV_SAVED        ((uint8_t)2U)
#define APP_BMS_SOC_OCV_SAVE_FAILED  ((uint8_t)3U)

void BMS_Task(void const *argument);
bool app_bms_force_soc_ocv(void);
uint8_t app_bms_soc_ocv_save_status(void);
void app_bms_soc_ocv_complete(void);
/* 排队请求，由BMS任务在唯一SOC写入上下文中校验并应用。 */
bool app_bms_soc_write_request(uint8_t requested_soc);
/* 查询SOC写入状态；成功或失败状态需由CAN回复后调用complete释放。 */
uint8_t app_bms_soc_write_status(void);
void app_bms_soc_write_complete(void);

#endif /* APP_BMS_TASK_H */
