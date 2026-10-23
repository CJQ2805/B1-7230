#include "boot_manage.h"

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Boot/boot_protocol.h"
#include "Boot/boot_upgrade.h"
#include "UDS/UDSBase/uds_port.h"
#include "Hal_Driver/boot_watchdog.h"
#include "SEGGER_RTT.h"
#include "n32g45x.h"

static uint8_t s_boot_state_checked; /* 非等待状态只处理一次，避免失败时重复刷屏或重复搬运。 */

void boot_manage_init(void)
{
    s_boot_state_checked = 0U;

    boot_protocol_init();
    (void)SEGGER_RTT_WriteString(0U, "\r\nBOOT start\r\n");
}

static void boot_manage_enter_wait_package(const char *reason)
{
    boot_watchdog_init();

    if (reason != 0)
    {
        (void)SEGGER_RTT_WriteString(0U, reason);
    }

    (void)boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_WAIT_PACKAGE);
}

static void boot_manage_jump_app1_or_wait(void)
{
    if (boot_app_jump_app1())
    {
        return;
    }

    boot_manage_enter_wait_package("BOOT APP1 invalid, wait package\r\n");
}

static void boot_manage_move_backup_then_jump(void)
{
    (void)SEGGER_RTT_WriteString(0U, "BOOT move APP2 to APP1\r\n");
    boot_watchdog_init();

    if (boot_upgrade_move_backup_to_app1())
    {
        (void)SEGGER_RTT_WriteString(0U, "BOOT move ok\r\n");
        (void)boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_APP_VALID);
        NVIC_SystemReset();
        return;
    }

    boot_manage_enter_wait_package("BOOT APP2 invalid, wait package\r\n");
}

void boot_manage_task(void)
{
    uint32_t flag;

    flag = boot_upgrade_get_flag();

    /* UDS提交搬运或APP1就绪状态后，先等肯定响应发送完再执行启动流程。 */
    if ((flag == BOOT_UPGRADE_FLAG_NEED_MOVE) &&
        (uds_reset_after_tx_pending() != 0U))
    {
        return;
    }
    if ((flag == BOOT_UPGRADE_FLAG_APP1_READY) &&
        (uds_reset_after_tx_pending() != 0U))
    {
        return;
    }

    if (flag == BOOT_UPGRADE_FLAG_WAIT_PACKAGE)
    {
        boot_watchdog_init();
        boot_protocol_task();
        return;
    }

    if (s_boot_state_checked != 0U)
    {
        return;
    }

    s_boot_state_checked = 1U;

    if ((flag == BOOT_UPGRADE_FLAG_DEFAULT) || (flag == BOOT_UPGRADE_FLAG_APP_VALID))
    {
        (void)SEGGER_RTT_WriteString(0U, "BOOT normal, jump APP1\r\n");
        boot_manage_jump_app1_or_wait();
        return;
    }

    if (flag == BOOT_UPGRADE_FLAG_NEED_MOVE)
    {
        boot_manage_move_backup_then_jump();
        return;
    }

    if (flag == BOOT_UPGRADE_FLAG_APP1_READY)
    {
        boot_watchdog_init();
        if (boot_upgrade_app1_ready_is_valid())
        {
            (void)SEGGER_RTT_WriteString(0U, "BOOT APP1 verified, jump\r\n");
            if (boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_APP_VALID))
            {
                NVIC_SystemReset();
                return;
            }
        }
        boot_manage_enter_wait_package("BOOT APP1 verify failed, wait package\r\n");
        NVIC_SystemReset();
        return;
    }

    boot_manage_enter_wait_package("BOOT flag unknown, wait package\r\n");
}

