#include "boot_uds.h"

#include "Boot/boot_config.h"
#include "Boot/boot_upgrade.h"
#include "SID10_SessionControl.h"
#include "SID27_SecurityAccess.h"
#include "SID28_CommunicationControl.h"
#include "SID85_ControlDTCSetting.h"
#include "uds_port.h"
#include "uds_upgrade.h"

static uint8_t s_boot_uds_started;

static bool boot_uds_start_if_waiting(void)
{
    if (s_boot_uds_started != 0U)
    {
        return true;
    }
    if (boot_upgrade_get_flag() != BOOT_UPGRADE_FLAG_WAIT_PACKAGE)
    {
        return false;
    }
    if (s_boot_uds_started == 0U)
    {
        uds_upgrade_reset_state();
        uds_security_reset();
        service_28_reset();
        service_85_reset();
        uds_init();
        set_current_session(UDS_SESSION_PROG);
        s_boot_uds_started = 1U;
    }
    return true;
}

void boot_uds_task_1ms(void)
{
    if (boot_uds_start_if_waiting())
    {
        uds_1ms_task();
    }
}

void boot_uds_receive_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if ((data != 0) &&
        (boot_upgrade_get_flag() == BOOT_UPGRADE_FLAG_WAIT_PACKAGE) &&
        boot_uds_start_if_waiting())
    {
        uds_recv_frame(can_id, (uint8_t *)data, dlc);
    }
}
