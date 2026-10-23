/*****************************************************************************
 * Copyright (c) 2019, Nations Technologies Inc.
 *
 * All rights reserved.
 * ****************************************************************************/

#ifndef __N32G45X_IT_H__
#define __N32G45X_IT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "n32g45x.h"

/* HardFault调试快照，异常后在Watch窗口查看，用于反查真实出错位置。 */
extern volatile uint32_t gu32_hardfault_r0;
extern volatile uint32_t gu32_hardfault_r1;
extern volatile uint32_t gu32_hardfault_r2;
extern volatile uint32_t gu32_hardfault_r3;
extern volatile uint32_t gu32_hardfault_r12;
extern volatile uint32_t gu32_hardfault_lr;
extern volatile uint32_t gu32_hardfault_pc;
extern volatile uint32_t gu32_hardfault_xpsr;
extern volatile uint32_t gu32_hardfault_cfsr;
extern volatile uint32_t gu32_hardfault_hfsr;
extern volatile uint32_t gu32_hardfault_dfsr;
extern volatile uint32_t gu32_hardfault_afsr;
extern volatile uint32_t gu32_hardfault_bfar;
extern volatile uint32_t gu32_hardfault_mmfar;
extern volatile uint32_t gu32_hardfault_exc_return;
extern volatile uint32_t gu32_hardfault_msp;
extern volatile uint32_t gu32_hardfault_psp;
extern volatile uint32_t gu32_hardfault_control;
extern volatile uint32_t gu32_hardfault_selected_sp;

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

/* 项目外部中断：MCU_SW接PD1/EXTI1；AFE_ALERT接PE11/EXTI11；低功耗时MCU_RX使用EXTI5。 */
void EXTI1_IRQHandler(void);
void EXTI9_5_IRQHandler(void);
void EXTI15_10_IRQHandler(void);
void RTC_WKUP_IRQHandler(void);

/* 调试串口实际使用USART2，其余声明保留给厂家启动文件弱符号覆盖。 */
void USART1_IRQHandler(void);
void USART2_IRQHandler(void);
void USART3_IRQHandler(void);
void UART4_IRQHandler(void);
void UART6_IRQHandler(void);
void UART7_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __N32G45X_IT_H__ */

