#ifndef __APP_LOW_POWER_MANAGE_H__
#define __APP_LOW_POWER_MANAGE_H__

#include <stdint.h>

#define Module_Major_Version_APP_LowPower    1
#define Module_Minor_Version_APP_LowPower    0

/*
 * APP低功耗10ms实时任务入口。
 *
 * 调用位置：
 * 由BMS_Task的10ms循环调用。
 *
 * 内部流程：
 * 1. 判断当前是否处于升级流程，升级中禁止低功耗。
 * 2. 判断电流是否为空闲状态，无明显充放电电流才允许继续累计。
 * 3. 判断串口收发是否空闲，通信活跃时重新计时。
 * 4. 连续满足条件达到配置空闲时间后进入低功耗；DETC、DETD均不在位时，转为统一软关机流程。
 * 5. 被RTC、AFE_ALERT或RX唤醒后，恢复时钟、GPIO、定时器、串口和AFE接口。
 *
 * elapsed_ms表示本次任务距上次调用的毫秒数，通常为10ms。
 * 注意：这个接口不应该在中断中调用。
 */
void app_low_power_task(uint32_t elapsed_ms);

/* 软关机流程期间禁止进入STOP；置0恢复普通低功耗判定。 */
void app_low_power_set_shutdown_inhibit(uint8_t inhibit);

#endif
