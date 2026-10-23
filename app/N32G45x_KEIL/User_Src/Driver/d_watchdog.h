#ifndef __D_WATCHDOG_H__
#define __D_WATCHDOG_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * APP 独立看门狗封装。
 * 说明：
 * 1. 正常运行时由系统监控任务在所有任务健康时喂狗，耗时擦写流程保留受控喂狗。
 * 2. STOP 低功耗前不关闭、不屏蔽 IWDG，先把重装窗口拉大并立即喂狗。
 * 3. STOP 通过RTC周期唤醒，唤醒后在恢复外设前单独立即喂狗。
 */
void d_watchdog_init(void);
void d_watchdog_feed(void);
void d_watchdog_pause(void);
void d_watchdog_resume(void);
void d_watchdog_tick_1ms(void);
bool d_watchdog_is_running(void);
uint32_t d_watchdog_get_counter_ms(void);
bool d_watchdog_enter_low_power(void);
void d_watchdog_exit_low_power(void);

#endif
