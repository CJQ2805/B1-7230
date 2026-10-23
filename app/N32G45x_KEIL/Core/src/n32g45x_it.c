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
 * @file n32g45x_it.c
 * @author Nations
 * @version v1.0.0
 *
 * @copyright Copyright (c) 2019, Nations Technologies Inc. All rights reserved.
 */
/*
 * 文件作用：
 *   APP工程的中断入口文件，负责把N32G45X启动文件中的异常和外设中断接入到本项目模块。
 *
 * 主要用途：
 *   1. 保存HardFault现场寄存器，方便Keil Watch窗口定位异常发生前的PC、LR和故障状态。
 *   2. 承接系统异常、定时器、串口、RTC唤醒和外部中断入口。
 *   3. 在中断中只做最小动作，把实际业务处理交给Driver、Board或App主循环任务。
 *
 * 适用范围：
 *   仅适用于当前N32G45X APP工程。更换MCU时，本文件属于芯片相关入口文件，应重新按新芯片
 *   的启动文件和中断向量表适配，上层业务模块不应该直接依赖本文件。
 *
 * 调用关系：
 *   启动文件根据中断向量自动进入本文件；本文件再调用d_uart、d_timer、d_exti、d_rtc等底层或
 *   抽象接口，把事件传递到主循环轮询任务。
 *
 * 注意事项：
 *   中断入口禁止执行耗时业务、Flash擦写、阻塞式IIC和复杂协议解析，避免破坏1ms系统时基。
 */
#include "n32g45x_it.h"
#include "main.h"
#include "d_exti.h"
#include "Driver/d_can.h"
#include "Driver/d_rtc.h"

/* HardFault调试快照：停在HardFault后，在Watch窗口查看这些变量即可定位出错前现场。 */
volatile uint32_t gu32_hardfault_r0;
volatile uint32_t gu32_hardfault_r1;
volatile uint32_t gu32_hardfault_r2;
volatile uint32_t gu32_hardfault_r3;
volatile uint32_t gu32_hardfault_r12;
volatile uint32_t gu32_hardfault_lr;
volatile uint32_t gu32_hardfault_pc;
volatile uint32_t gu32_hardfault_xpsr;
volatile uint32_t gu32_hardfault_cfsr;
volatile uint32_t gu32_hardfault_hfsr;
volatile uint32_t gu32_hardfault_dfsr;
volatile uint32_t gu32_hardfault_afsr;
volatile uint32_t gu32_hardfault_bfar;
volatile uint32_t gu32_hardfault_mmfar;
volatile uint32_t gu32_hardfault_exc_return;
volatile uint32_t gu32_hardfault_msp;
volatile uint32_t gu32_hardfault_psp;
volatile uint32_t gu32_hardfault_control;
volatile uint32_t gu32_hardfault_selected_sp;

/** @addtogroup N32G45X_StdPeriph_Template
 * @{
 */

/******************************************************************************/
/*            Cortex-M4 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
 * @brief  This function handles NMI exception.
 */
void NMI_Handler(void)
{
}

/**
 * @brief  This function handles Hard Fault exception.
 */
__attribute__((naked)) void HardFault_Handler(void)
{
    __ASM volatile
    (
        "MOV r1, lr\n"
        "MRS r2, MSP\n"
        "MRS r3, PSP\n"
        "TST r1, #4\n"
        "ITE EQ\n"
        "MOVEQ r0, r2\n"
        "MOVNE r0, r3\n"
        "B HardFault_C_Handler\n"
    );
}

void HardFault_C_Handler(uint32_t *fault_stack, uint32_t exc_return, uint32_t msp, uint32_t psp)
{
    /* 保存异常入口时的栈和返回信息，判断现场来自MSP还是PSP。 */
    gu32_hardfault_exc_return = exc_return;
    gu32_hardfault_msp = msp;
    gu32_hardfault_psp = psp;
    gu32_hardfault_control = __get_CONTROL();
    gu32_hardfault_selected_sp = (uint32_t)fault_stack;

    /* fault_stack[6]是进入HardFault前的PC，优先用它反查map文件定位具体函数。 */
    gu32_hardfault_r0    = fault_stack[0];
    gu32_hardfault_r1    = fault_stack[1];
    gu32_hardfault_r2    = fault_stack[2];
    gu32_hardfault_r3    = fault_stack[3];
    gu32_hardfault_r12   = fault_stack[4];
    gu32_hardfault_lr    = fault_stack[5];
    gu32_hardfault_pc    = fault_stack[6];
    gu32_hardfault_xpsr  = fault_stack[7];
    gu32_hardfault_cfsr  = SCB->CFSR;
    gu32_hardfault_hfsr  = SCB->HFSR;
    gu32_hardfault_dfsr  = SCB->DFSR;
    gu32_hardfault_afsr  = SCB->AFSR;
    gu32_hardfault_bfar  = SCB->BFAR;
    gu32_hardfault_mmfar = SCB->MMFAR;

    while (1)
    {
    }
}

/**
 * @brief  This function handles Memory Manage exception.
 */
void MemManage_Handler(void)
{
    /* Go to infinite loop when Memory Manage exception occurs */
    while (1)
    {
    }
}

/**
 * @brief  This function handles Bus Fault exception.
 */
void BusFault_Handler(void)
{
    /* Go to infinite loop when Bus Fault exception occurs */
    while (1)
    {
    }
}

/**
 * @brief  This function handles Usage Fault exception.
 */
void UsageFault_Handler(void)
{
    /* Go to infinite loop when Usage Fault exception occurs */
    while (1)
    {
    }
}

/**
 * @brief  This function handles Debug Monitor exception.
 */
void DebugMon_Handler(void)
{
}

void EXTI1_IRQHandler(void)
{
    /* MCU_SW接PD1/EXTI1；必须进入驱动回调，避免落入启动文件的弱默认死循环。 */
    d_exti_irq_handler(D_GPIO_PIN(1U));
}

void EXTI15_10_IRQHandler(void)
{
    /* EXTI10~15共用IRQ，逐线检查并清除；当前AFE_ALERT接PE11/EXTI11。 */
    d_exti_irq_handler(D_GPIO_PIN(10U));
    d_exti_irq_handler(D_GPIO_PIN(11U));
    d_exti_irq_handler(D_GPIO_PIN(12U));
    d_exti_irq_handler(D_GPIO_PIN(13U));
    d_exti_irq_handler(D_GPIO_PIN(14U));
    d_exti_irq_handler(D_GPIO_PIN(15U));
}

void EXTI9_5_IRQHandler(void)
{
    /* EXTI5~9共用IRQ；STOP时还包含CAN_WK(8)和DETD(6)，必须逐线清挂起。 */
    d_exti_irq_handler(D_GPIO_PIN(5U));
    d_exti_irq_handler(D_GPIO_PIN(6U));
    d_exti_irq_handler(D_GPIO_PIN(7U));
    d_exti_irq_handler(D_GPIO_PIN(8U));
    d_exti_irq_handler(D_GPIO_PIN(9U));
}

void RTC_WKUP_IRQHandler(void)
{
    /* RTC WakeUp只清硬件挂起并置软件标志，耗时恢复动作放到主循环。 */
    d_rtc_wakeup_irq_handler();
}

void CAN1_SCE_IRQHandler(void)
{
    d_can_error_irq_handler();
}

/**
 * @}
 */

