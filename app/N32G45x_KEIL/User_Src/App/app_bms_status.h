#ifndef __APP_BMS_STATUS_H__
#define __APP_BMS_STATUS_H__

#include <stdint.h>

#define APP_BMS_STATUS_STATIC      ((uint8_t)0U)
#define APP_BMS_STATUS_DISCHARGE   ((uint8_t)1U)
#define APP_BMS_STATUS_CHARGE      ((uint8_t)2U)
#define APP_BMS_STATUS_FEEDBACK    ((uint8_t)3U)

/* DataHandle_Task在更新统一快照后每10ms调用一次；结果写入快照的bms_work_status。 */
void app_bms_status_task(void);

#endif
