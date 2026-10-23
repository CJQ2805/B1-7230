#include "n32g45x_it.h"

#include "Hal_Driver/boot_timer.h"
#include "Hal_Driver/boot_can.h"

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
    while (1)
    {
    }
}

void MemManage_Handler(void)
{
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    while (1)
    {
    }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

void PendSV_Handler(void)
{
}

void SysTick_Handler(void)
{
}

void TIM6_IRQHandler(void)
{
    boot_timer_irq_handler();
}

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    boot_can_irq_handler();
}

void CAN1_SCE_IRQHandler(void)
{
    boot_can_error_irq_handler();
}

