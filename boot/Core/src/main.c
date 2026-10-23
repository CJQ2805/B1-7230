#include "main.h"

#include "Hal_Driver/boot_led.h"
#include "Boot/boot_manage.h"
#include "Hal_Driver/boot_timer.h"
#include "Hal_Driver/boot_uart.h"
#include "Hal_Driver/boot_watchdog.h"
#include "Hal_Driver/boot_can.h"
#include "UDS/UDSLogic/boot_uds.h"
#include "misc.h"

/*
 * 文件作用：
 *   BOOT工程主入口，负责初始化BOOT最小运行环境并循环处理升级状态机。
 *
 * 主要用途：
 *   1. 初始化中断优先级、BOOT定时器、升级串口、运行指示LED和BOOT管理模块。
 *   2. 在主循环中持续处理升级协议、APP2搬运APP1和默认跳转APP1的状态机。
 *   3. 在BOOT等待升级或搬运升级期间喂狗，避免升级流程卡死。
 *
 * 适用范围：
 *   仅适用于当前N32G45X BOOT工程。换芯片时启动文件、中断入口、时钟和底层驱动需要重新适配。
 *
 * 调用关系：
 *   启动文件进入 main()；main() 调用Boot和Hal_Driver层接口，不直接实现协议解析和Flash搬运细节。
 *
 * 注意事项：
 *   BOOT中LED保持常亮用于运行区分；默认上电能跳APP时不启动IWDG，避免把BOOT看门狗带入APP。
 */
 
 
void boot_gpio_init(void)
{
	GPIO_InitType gpio_init;

	RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOC, ENABLE);
	RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOE, ENABLE);
	
	GPIO_InitStruct(&gpio_init);
	gpio_init.Pin = GPIO_PIN_4;
	gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
	gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_InitPeripheral(GPIOC, &gpio_init);

	GPIO_WriteBit(GPIOC, GPIO_PIN_4, Bit_SET);

	gpio_init.Pin = GPIO_PIN_7;
	GPIO_InitPeripheral(GPIOE, &gpio_init);		
	GPIO_WriteBit(GPIOE, GPIO_PIN_7, Bit_SET);	
	
	gpio_init.Pin = GPIO_PIN_13;
	GPIO_InitPeripheral(GPIOE, &gpio_init);		
	GPIO_WriteBit(GPIOE, GPIO_PIN_13, Bit_SET);		
	
	gpio_init.Pin = GPIO_PIN_12;
	GPIO_InitPeripheral(GPIOE, &gpio_init);		
	GPIO_WriteBit(GPIOE, GPIO_PIN_12, Bit_SET);		
}
 
int main(void)
{
    uint32_t uds_last_tick;

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_0);

    boot_led_init();
	boot_gpio_init();
    boot_led_on();
    boot_timer_init();
    boot_uart_init();
    boot_can_init();
    boot_manage_init();
    uds_last_tick = boot_timer_get_ms();

    while (1)
    {
        boot_manage_task();
        boot_can_task();
        if ((uint32_t)(boot_timer_get_ms() - uds_last_tick) != 0UL)
        {
            uds_last_tick = boot_timer_get_ms();
            boot_uds_task_1ms();
        }
        boot_watchdog_feed();
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(const uint8_t* expr, const uint8_t* file, uint32_t line)
{
    (void)expr;
    (void)file;
    (void)line;

    while (1)
    {
    }
}
#endif

