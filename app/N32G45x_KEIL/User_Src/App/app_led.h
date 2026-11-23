#ifndef __APP_LED_H__
#define __APP_LED_H__

#include <stdint.h>

/* 5颗LD灯按20%一个档位显示当前显示SOC，最低档从LED5开始向LED1递增。 */
#define APP_LED_SOC_STEP_PERCENT       20U
#define APP_LED_SOC_MAX_PERCENT        100U
#define APP_LED_LEVEL_COUNT            5U
#define APP_LED_TASK_PERIOD_MS         10U
#define APP_LED_LOW_SOC_THRESHOLD_PERCENT 10U
#define APP_LED_BLINK_INTERVAL_MS      500U
#define APP_LED_BLINK_COUNT             \
    (APP_LED_BLINK_INTERVAL_MS / APP_LED_TASK_PERIOD_MS)
#define APP_LED_LOW_SOC_BLINK_INTERVAL_MS 250U
#define APP_LED_LOW_SOC_BLINK_COUNT      \
    (APP_LED_LOW_SOC_BLINK_INTERVAL_MS / APP_LED_TASK_PERIOD_MS)
/* LD1~LD5由PNP高边管驱动，MCU输出低电平点亮，高电平熄灭。 */
#define APP_LED_ON_LEVEL               0U
#define APP_LED_OFF_LEVEL              1U

/* 初始化LD1~LD5显示输出，默认全部熄灭。 */
void app_led_init(void);

/* 10ms业务调用：故障码轮播优先，之后按充电/DETC/按键窗口显示SOC。 */
void app_led_task(void);

/* 故障码轮播或轮播后的8秒SOC显示期间返回1，低功耗须保持阻止。 */
uint8_t app_led_holds_awake(void);

#endif
