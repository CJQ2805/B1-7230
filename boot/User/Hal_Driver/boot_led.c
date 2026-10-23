/*
 * 文件作用：
 *   BOOT工程运行指示LED驱动，负责初始化和控制新版原理图中的LED引脚。
 *
 * 主要用途：
 *   1. 在BOOT启动后将LED置为常亮，方便调试时区分当前程序运行在BOOT区还是APP区。
 *   2. 把PC13端口、有效电平和GPIO初始化细节集中在BOOT底层驱动中。
 *
 * 适用范围：
 *   仅适用于当前N32G45X BOOT工程和当前原理图LED连接方式。更换MCU或原理图后优先修改本文件。
 *
 * 调用关系：
 *   boot main初始化阶段调用 boot_led_init() 和 boot_led_on()；本文件内部调用N32 GPIO/RCC接口。
 *
 * 注意事项：
 *   BOOT中LED只做常亮状态指示，不参与升级协议和Flash搬运逻辑，避免影响升级流程。
 */
#include "Hal_Driver/boot_led.h"

#include "n32g45x_gpio.h"
#include "n32g45x_rcc.h"

#define BOOT_LED_PORT       GPIOC
#define BOOT_LED_PIN        GPIO_PIN_13
#define BOOT_LED_ON_LEVEL   0U
#define BOOT_LED_OFF_LEVEL  1U

void boot_led_set(uint8_t level)
{
    GPIO_WriteBit(BOOT_LED_PORT, BOOT_LED_PIN, (level != 0U) ? Bit_SET : Bit_RESET);
}

void boot_led_init(void)
{
    GPIO_InitType gpio_init;

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOC, ENABLE);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = BOOT_LED_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitPeripheral(BOOT_LED_PORT, &gpio_init);

    boot_led_set(BOOT_LED_OFF_LEVEL);
}

void boot_led_on(void)
{
    boot_led_set(BOOT_LED_ON_LEVEL);
}

