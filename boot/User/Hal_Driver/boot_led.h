#ifndef __BOOT_LED_H__
#define __BOOT_LED_H__

#include <stdint.h>

/* 初始化BOOT运行指示LED。 */
void boot_led_init(void);
/* 设置BOOT运行指示LED电平。 */
void boot_led_set(uint8_t level);
/* BOOT停留或执行升级时LED常亮，便于区分当前运行在BOOT区。 */
void boot_led_on(void);

#endif
