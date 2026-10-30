/*****************************************************************************
 * Copyright (c) 2019, Nations Technologies Inc.
 *
 * All rights reserved.
 * ****************************************************************************
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the disclaimer below.
 *
 * Nations' name may not be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * DISCLAIMER: THIS SOFTWARE IS PROVIDED BY NATIONS "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * DISCLAIMED. IN NO EVENT SHALL NATIONS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ****************************************************************************/

/**
 * @file main.c
 * @author Nations
 * @version v1.0.0
 *
 * @copyright Copyright (c) 2019, Nations Technologies Inc. All rights reserved.
 */
/*
 * 文件作用：
 *   APP工程主入口，负责系统初始化、周期任务调度和主循环喂狗。
 *
 * 主要用途：
 *   1. 初始化时钟、定时器、板级IO、串口、RTC、APP业务模块和看门狗。
 *   2. 创建FreeRTOS任务；业务周期和调度均由各自Task负责。
 *   3. 作为组合根挂接业务快照层，但不在 main.c 中写具体业务判断。
 *
 * 适用范围：
 *   仅适用于当前 APP 工程。芯片启动文件、中断入口和硬件驱动细节不应该在这里扩散。
 *
 * 调用关系：
 *   main.c 调用各业务模块入口；业务模块内部自己维护状态。统一业务快照由
 *   app_bms_snapshot_update() 刷新，后续模块逐步改为读取快照。
 *
 * 注意事项：
 *   main.c 只保留任务接口和调度顺序，不把故障、MOS、LOG、通信等处理细节写回主循环。
 */
#include <stdio.h>
#include "main.h"
#include "Driver/d_timer.h"
#include "Driver/d_system.h"
#include "Driver/d_uart.h"
#include "App/app_log_manage.h"
#include "App/app_bms_fault.h"
#include "App/app_h_power_manage.h"
#include "App/app_power_manage.h"
#include "Protocol/Comm/ducar_uart_protocol.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#include "App/app_mcu_ntc.h"
#include "App/app_input_signals.h"
#include "App/app_led.h"
#include "App/app_bms_snapshot.h"
#include "App/app_long_time_undervoltage_shutdown.h"
#include "App/app_current_calib.h"
#include "App/app_can.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "Board/board_io.h"
#include "Driver/d_flash.h"
#include "Driver/d_flash_spi.h"
#include "Driver/d_rtc.h"
#include "Driver/d_watchdog.h"
#include "HSI_afe/hsi_afe.h"
#include "SEGGER_RTT.h"
#include "Task/app_freertos.h"
#include "cmsis_os.h"

/** @addtogroup N32G45X_StdPeriph_Examples
 * @{
 */

/** @addtogroup USART_Interrupt
 * @{
 */

/**
 * @brief  Main program
 */
int main(void)
{	
    __enable_irq();
    SEGGER_RTT_Init();
    app_log_capture_reset_flags(); /* 必须在外设初始化前读走RCC复位标志，并立即输出到RTT。 */
    /* main只负责初始化和调度，具体业务逻辑放在User各模块中。 */
    /* FreeRTOS requires all implemented priority bits to be pre-emption bits. */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);
    /* d_timer_init内部统一初始化TIM6和TIM7的1ms硬件时基。 */
    d_timer_init();
    /* 板级IO必须先初始化，后续UART/AFE/MOS模块才可使用原理图引脚。 */
    board_io_init();
    d_flash_spi_init();
    /* SN、电压偏差和后续业务Flash访问都需要资源守卫，先建立Flash互斥资源。 */
    if (!d_flash_init())
    {
        __disable_irq();
        for (;;)
        {
            NVIC_SystemReset();
        }
    }
    /* 先按“SN”标识恢复制造SN及电压偏差，再初始化串口和后续业务模块。 */
    app_param_init();
    d_uart_init();
    /* AFE采样、MOS控制、均衡和快照读取共享同一硬件/数据资源，任务创建前先建立守卫。 */
    if (!hsi_afe_resource_init())
    {
        __disable_irq();
        for (;;)
        {
            NVIC_SystemReset();
        }
    }
    /* RTC使用外部32.768kHz LSE晶振，初始化失败时LOG会退回运行时间。 */
    d_rtc_init();
    app_mcu_ntc_init();
    app_input_signals_init();
    app_bms_snapshot_init();
    app_led_init();
    /* 电流校准必须在AFE对象建立后初始化，默认K=1、B=0并读取Flash参数。 */
    app_current_calib_init();
    app_bms_snapshot_update();
    /* 上电恢复shutdown/复位/升级前保存的PF锁存来源，避免PF类故障掉电丢失。 */
    app_bms_fault_load_shutdown_pf();
    /* 初始化高压/MOS管理模块，端口来自Board层，启动时默认输出安全关闭状态。 */
    (void)app_h_power_manage_init();
    app_log_init();
    app_power_manage_init();
    app_long_time_undervoltage_shutdown_init();
    ducar_uart_protocol_init();
    ducar_iap_protocol_init();
    (void)app_can_init();
    d_watchdog_init();

    /* 看门狗未成功启动时禁止进入业务调度，避免系统裸奔；复位后重新尝试初始化。 */
    if (!d_watchdog_is_running())
    {
        __disable_irq();
        for (;;)
        {
            NVIC_SystemReset();
        }
    }

	SEGGER_RTT_printf(0,"App init !!\r\n");		
    /* 初始化调试快照；后续各任务完成后按模块刷新对应小结构体。 */

    /* 启动FreeRTOS任务，业务调度由各任务自身负责。 */
    app_freertos_init();
    osKernelStart();

    /* 调度器启动后不会返回到main。 */
    while (1)
    {
    }
}

#ifdef USE_FULL_ASSERT

/**
 * @brief  Reports the name of the source file and the source line number
 *         where the assert_param error has occurred.
 * @param file pointer to the source file name
 * @param line assert_param error line source number
 */
void assert_failed(const uint8_t* expr, const uint8_t* file, uint32_t line)
{
    /* User can add his own implementation to report the file name and line number,
       ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

    /* Infinite loop */
    while (1)
    {
    }
}

#endif

/**
 * @}
 */

/**
 * @}
 */

