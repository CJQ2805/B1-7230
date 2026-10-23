#ifndef __APP_CHARGE_TIME_H__
#define __APP_CHARGE_TIME_H__

#include <stdint.h>

/* 剩余充电时间单位为分钟；未充电或尚无有效估算时返回此值。 */
#define APP_CHARGE_TIME_INVALID_MINUTES ((uint16_t)0xFFFFU)

/* 由BMS任务周期调用；输入来自最近一次统一业务快照。 */
void app_charge_time_task(void);

/* 读取最近一次计算的剩余充电时间。 */
uint16_t app_charge_time_get_minutes(void);

#endif
