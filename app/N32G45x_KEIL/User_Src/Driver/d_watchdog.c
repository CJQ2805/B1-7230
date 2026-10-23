/*
 * 文件作用：
 *   N32G45X独立看门狗驱动实现，封装IWDG初始化、喂狗和休眠相关复位策略。
 *
 * 主要用途：
 *   1. 系统监控任务在任务健康时周期喂狗。
 *   2. Flash擦写和IAP长操作期间提供喂狗接口。
 *   3. 低功耗期间保持IWDG有效，由RTC周期唤醒后单独喂狗。
 *
 * 适用范围：
 *   只适用于当前N32G45X IWDG设计。更换MCU时保留 d_watchdog.h，对本文件重新实现。
 *
 * 调用关系：
 *   main.c 初始化；系统监控、IAP、Flash擦写和低功耗模块调用相关接口。
 *
 * 注意事项：
 *   IWDG启用后不能真正关闭，低功耗流程不能清除IWDGRSTEN。
 */
#include "Driver/d_watchdog.h"

#include "n32g45x_iwdg.h"
#include "n32g45x_pwr.h"
#include "n32g45x_rcc.h"

#define D_WATCHDOG_LSI_READY_TIMEOUT        ((uint32_t)100000UL) /* 等待LSI起振的超时计数，避免硬件异常时死等。 */
#define D_WATCHDOG_STATUS_UPDATE_TIMEOUT    ((uint32_t)100000UL) /* 等待IWDG预分频/重装载同步完成的超时计数。 */
#define D_WATCHDOG_LOW_POWER_RELOAD_VALUE   ((uint16_t)0x0FFFU)  /* 低功耗前拉到最大约26s，RTC唤醒后先喂狗再恢复复位能力。 */

static volatile uint8_t s_d_watchdog_started;        /* IWDG是否已经由APP启动。 */
static volatile uint32_t s_d_watchdog_feed_count;    /* 调试观察用：累计喂狗次数。 */

static bool d_watchdog_enable_lsi(void)
{
    uint32_t u32TimeoutCount = D_WATCHDOG_LSI_READY_TIMEOUT;

    RCC_EnableLsi(ENABLE);
    while ((RCC_GetFlagStatus(RCC_FLAG_LSIRD) == RESET) && (u32TimeoutCount > 0UL))
    {
        u32TimeoutCount--;
    }

    return (RCC_GetFlagStatus(RCC_FLAG_LSIRD) != RESET);
}

static bool d_watchdog_wait_update_done(void)
{
    uint32_t u32TimeoutCount = D_WATCHDOG_STATUS_UPDATE_TIMEOUT;

    while (((IWDG_GetStatus(IWDG_PVU_FLAG) != RESET) || (IWDG_GetStatus(IWDG_CRVU_FLAG) != RESET)) &&
           (u32TimeoutCount > 0UL))
    {
        u32TimeoutCount--;
    }

    return ((IWDG_GetStatus(IWDG_PVU_FLAG) == RESET) &&
            (IWDG_GetStatus(IWDG_CRVU_FLAG) == RESET));
}

static bool d_watchdog_config_reload(uint16_t reload)
{
    IWDG_WriteConfig(IWDG_WRITE_ENABLE);
    if (!d_watchdog_wait_update_done())
    {
        return false;
    }

    IWDG_SetPrescalerDiv(IWDG_PRESCALER_DIV256);
    IWDG_CntReload(reload);
    if (!d_watchdog_wait_update_done())
    {
        return false;
    }

    IWDG_ReloadKey();
    return true;
}

static void d_watchdog_enable_reset(void)
{
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_PWR, ENABLE);

    /*
     * 正常运行时允许IWDG超时复位。
     * IWDGRSTEN置1：IWDG超时后产生系统复位。
     * IWDGWPEN清0：不使用IWDG超时唤醒低功耗。
     */
    PWR->CTRL2 |= (uint32_t)PWR_CTRL2_IWDGRSTEN;
    PWR->CTRL2 &= (uint32_t)(~PWR_CTRL2_IWDGWPEN);
}

void d_watchdog_init(void)
{
    if (s_d_watchdog_started != 0U)
    {
        d_watchdog_feed();
        return;
    }

    if (!d_watchdog_enable_lsi())
    {
        return;
    }

    d_watchdog_enable_reset();
    if (!d_watchdog_config_reload(D_WATCHDOG_LOW_POWER_RELOAD_VALUE))
    {
        return;
    }

    IWDG_Enable();
    s_d_watchdog_started = 1U;
}

void d_watchdog_feed(void)
{
    if (s_d_watchdog_started != 0U)
    {
        IWDG_ReloadKey();
        s_d_watchdog_feed_count++;
    }
}

void d_watchdog_pause(void)
{
    (void)d_watchdog_enter_low_power();
}

void d_watchdog_resume(void)
{
    d_watchdog_exit_low_power();
}

void d_watchdog_tick_1ms(void)
{
    /* IWDG由独立LSI时钟硬件计数，不再需要1ms中断推进软件计数。 */
}

bool d_watchdog_is_running(void)
{
    return (s_d_watchdog_started != 0U);
}

uint32_t d_watchdog_get_counter_ms(void)
{
    return s_d_watchdog_feed_count;
}

bool d_watchdog_enter_low_power(void)
{
    /*
     * 如果上层还没初始化IWDG，这里尝试初始化一次。
     * 这样低功耗流程不用关心看门狗是否已经启动。
     */
    if (s_d_watchdog_started == 0U)
    {
        d_watchdog_init();
    }

    if (s_d_watchdog_started == 0U)
    {
        return false;
    }

    /* 低功耗仍保持IWDG超时复位能力，不能清除IWDGRSTEN。 */
    d_watchdog_enable_reset();
    if (!d_watchdog_config_reload(D_WATCHDOG_LOW_POWER_RELOAD_VALUE))
    {
        /* 如果低功耗配置失败，立即恢复正常IWDG复位能力并喂狗，避免系统裸跑。 */
        d_watchdog_enable_reset();
        d_watchdog_feed();
        return false;
    }

    return true;
}

void d_watchdog_exit_low_power(void)
{
    if (s_d_watchdog_started == 0U)
    {
        return;
    }

    /* STOP唤醒后由低功耗路径单独立即喂狗，再恢复IWDG复位能力。 */
    d_watchdog_feed();
    d_watchdog_enable_reset();
    (void)d_watchdog_config_reload(D_WATCHDOG_LOW_POWER_RELOAD_VALUE);
}

