#include "Hal_Driver/boot_watchdog.h"

#include "n32g45x_iwdg.h"
#include "n32g45x_rcc.h"

#define BOOT_WATCHDOG_LSI_READY_TIMEOUT    ((uint32_t)100000UL)
#define BOOT_WATCHDOG_STATUS_TIMEOUT       ((uint32_t)100000UL)
#define BOOT_WATCHDOG_RELOAD_VALUE         ((uint16_t)3124U)

static uint8_t s_boot_watchdog_started; /* 只在BOOT等待升级或搬运升级时启动，避免默认跳APP时把IWDG带进APP。 */

static bool boot_watchdog_wait_update_done(void)
{
    uint32_t timeout = BOOT_WATCHDOG_STATUS_TIMEOUT;

    while (((IWDG_GetStatus(IWDG_PVU_FLAG) != RESET) || (IWDG_GetStatus(IWDG_CRVU_FLAG) != RESET)) &&
           (timeout > 0UL))
    {
        timeout--;
    }

    return (timeout > 0UL);
}

void boot_watchdog_feed(void)
{
    if (s_boot_watchdog_started != 0U)
    {
        IWDG_ReloadKey();
    }
}

void boot_watchdog_init(void)
{
    uint32_t timeout = BOOT_WATCHDOG_LSI_READY_TIMEOUT;

    if (s_boot_watchdog_started != 0U)
    {
        boot_watchdog_feed();
        return;
    }

    RCC_EnableLsi(ENABLE);
    while ((RCC_GetFlagStatus(RCC_FLAG_LSIRD) == RESET) && (timeout > 0UL))
    {
        timeout--;
    }

    if (timeout == 0UL)
    {
        return;
    }

    IWDG_WriteConfig(IWDG_WRITE_ENABLE);
    IWDG_SetPrescalerDiv(IWDG_PRESCALER_DIV256);
    IWDG_CntReload(BOOT_WATCHDOG_RELOAD_VALUE);

    if (!boot_watchdog_wait_update_done())
    {
        return;
    }

    IWDG_ReloadKey();
    IWDG_Enable();
    s_boot_watchdog_started = 1U;
}

bool boot_watchdog_is_started(void)
{
    return (s_boot_watchdog_started != 0U);
}

