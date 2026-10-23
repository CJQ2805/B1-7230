/*
 * 文件作用：
 *   N32G45X系统控制驱动实现，封装复位、低功耗、时钟恢复和复位原因读取。
 *
 * 主要用途：
 *   1. 给IAP流程提供软件复位和跳转前系统动作。
 *   2. 给低功耗模块提供STOP进入和唤醒后时钟恢复。
 *   3. 给LOG模块提供复位原因记录来源。
 *
 * 适用范围：
 *   只适用于当前N32G45X芯片。更换MCU时保留 d_system.h，对本文件重新实现。
 *
 * 调用关系：
 *   IAP、低功耗、LOG等模块调用 d_system_xxx()；本文件内部调用CMSIS和N32 PWR/RCC接口。
 *
 * 注意事项：
 *   进入低功耗或复位前必须确认通信发送、Flash操作和看门狗策略已经处理完成。
 */
#include "Driver/d_system.h"

#include "n32g45x.h"
#include "n32g45x_pwr.h"
#include "n32g45x_rcc.h"

extern void SystemInit(void);

bool d_system_switch_to_hsi(uint32_t u32TimeoutLimit)
{
    uint32_t u32WaitTimeout = u32TimeoutLimit;

    /*
     * STOP前先打开HSI。
     * 当前APP正常运行时默认使用HSE经PLL生成144MHz系统时钟，进入STOP前先切回HSI，方便STOP唤醒后重新初始化PLL。
     */
    RCC_EnableHsi(ENABLE);
    while ((RCC_GetFlagStatus(RCC_FLAG_HSIRD) == RESET) && (u32WaitTimeout > 0UL))
    {
        u32WaitTimeout--;
    }

    if (RCC_GetFlagStatus(RCC_FLAG_HSIRD) == RESET)
    {
        return false;
    }

    u32WaitTimeout = u32TimeoutLimit;
    /* 选择HSI作为系统时钟源；RCC_GetSysclkSrc()返回0x00表示当前系统时钟已经是HSI。 */
    RCC_ConfigSysclk(RCC_SYSCLK_SRC_HSI);
    while ((RCC_GetSysclkSrc() != 0x00U) && (u32WaitTimeout > 0UL))
    {
        u32WaitTimeout--;
    }

    return (RCC_GetSysclkSrc() == 0x00U);
}

void d_system_enter_stop0(void)
{
    /*
     * 进入STOP0。
     * PWR_REGULATOR_ON表示STOP期间主调压器保持开启，唤醒速度更快，功耗比深度关断略高。
     * PWR_STOPENTRY_WFI表示执行WFI后进入STOP，唤醒源由RTC/EXTI等中断事件触发。
     */
    PWR_EnterStopState(PWR_REGULATOR_ON, PWR_STOPENTRY_WFI);
}

void d_system_restore_clock(void)
{
    /*
     * STOP唤醒后，硬件时钟会回到较基础状态。
     * 这里复用启动时钟初始化流程，把系统恢复到工程默认的144MHz运行频率。
     */
    SystemInit();
}

uint32_t d_system_get_vector_table_addr(void)
{
    return SCB->VTOR;
}

void d_system_reset(void)
{
    NVIC_SystemReset();
}

uint32_t d_system_get_core_clock_hz(void)
{
    return SystemCoreClock;
}

void d_system_get_reset_flags(d_system_reset_flags_t *flags)
{
    uint32_t u32RawResetFlags;

    if (flags == 0)
    {
        return;
    }

    u32RawResetFlags = RCC->CTRLSTS;
    flags->raw_flags  = u32RawResetFlags;
    flags->bor_reset  = ((u32RawResetFlags & RCC_CTRLSTS_BORRSTF) != 0UL) ? 1U : 0U;
    flags->pin_reset  = ((u32RawResetFlags & RCC_CTRLSTS_PINRSTF) != 0UL) ? 1U : 0U;
    flags->por_reset  = ((u32RawResetFlags & RCC_CTRLSTS_PORRSTF) != 0UL) ? 1U : 0U;
    flags->soft_reset = ((u32RawResetFlags & RCC_CTRLSTS_SFTRSTF) != 0UL) ? 1U : 0U;
    flags->iwdg_reset = ((u32RawResetFlags & RCC_CTRLSTS_IWDGRSTF) != 0UL) ? 1U : 0U;
    flags->wwdg_reset = ((u32RawResetFlags & RCC_CTRLSTS_WWDGRSTF) != 0UL) ? 1U : 0U;
    flags->lpwr_reset = ((u32RawResetFlags & RCC_CTRLSTS_LPWRRSTF) != 0UL) ? 1U : 0U;

    /* 复位来源已经由上层保存，清除硬件锁存，避免后续正常启动重复判定为IWDG复位。 */
    RCC_ClrFlag();
}

