#ifndef __BOOT_WATCHDOG_H__
#define __BOOT_WATCHDOG_H__

#include <stdbool.h>

/*
 * BOOT 硬件独立看门狗。
 * BOOT 不进入低功耗，升级接收、擦写、搬运过程中使用 IWDG 防止卡死。
 */
void boot_watchdog_init(void);
void boot_watchdog_feed(void);
bool boot_watchdog_is_started(void);

#endif
