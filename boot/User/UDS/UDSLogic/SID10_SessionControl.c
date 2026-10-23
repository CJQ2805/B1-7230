#include "SID10_SessionControl.h"

#include "SID27_SecurityAccess.h"
#include "SID28_CommunicationControl.h"
#include "SID85_ControlDTCSetting.h"
#include "service_cfg.h"
#include "uds_service.h"
#include "uds_tp.h"
#include "uds_upgrade.h"

#define BOOT_UDS_P2_SERVER_MS       100U
#define BOOT_UDS_P2STAR_SERVER_MS   4000U

static uds_session_t s_current_session = UDS_SESSION_PROG;

void set_current_session(uds_session_t session)
{
    s_current_session = session;
}

uds_session_t get_current_session(void)
{
    return s_current_session;
}

bool_t service_10_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 2U)) ? TRUE : FALSE;
}

void service_10_SessionControl(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t response[6];
    uds_session_t current_session;

    (void)msg_dlc;
    subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);
    if ((subfunction != UDS_SESSION_STD) &&
        (subfunction != UDS_SESSION_PROG) &&
        (subfunction != UDS_SESSION_EXT))
    {
        uds_negative_rsp(SID_10, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    current_session = get_current_session();
    if (subfunction == UDS_SESSION_PROG)
    {
        /* Mirror APP handoff checks, but remain in BOOT instead of resetting. */
        if (g_tatype == N_TATYPE_FUNCTIONAL)
        {
            return;
        }
        if (current_session != UDS_SESSION_EXT)
        {
            uds_negative_rsp(SID_10, NRC_CONDITIONS_NOT_CORRECT);
            return;
        }
        if (get_current_sa_lv() < UDS_SA_LV1)
        {
            uds_negative_rsp(SID_10, NRC_SECURITY_ACCESS_DENIED);
            return;
        }
        if (UDS_GET_SUB_FUNCTION_SUPPRESS_POSRSP(msg_buf[1]) != 0U)
        {
            uds_negative_rsp(SID_10, NRC_SUBFUNCTION_NOT_SUPPORTED);
            return;
        }

        response[0] = USD_GET_POSITIVE_RSP(SID_10);
        response[1] = subfunction;
        response[2] = (uint8_t)(BOOT_UDS_P2_SERVER_MS >> 8);
        response[3] = (uint8_t)BOOT_UDS_P2_SERVER_MS;
        response[4] = (uint8_t)(BOOT_UDS_P2STAR_SERVER_MS >> 8);
        response[5] = (uint8_t)BOOT_UDS_P2STAR_SERVER_MS;
        if (uds_positive_rsp(response, sizeof(response)) != 0)
        {
            return;
        }

        /* WAIT_PACKAGE is persistent; only the UDS session changes in place. */
        set_current_session(UDS_SESSION_PROG);
        uds_security_reset();
        uds_upgrade_reset_state();
        service_28_reset();
        service_85_reset();
        return;
    }

    set_current_session((uds_session_t)subfunction);
    if (current_session != (uds_session_t)subfunction)
    {
        uds_security_reset();
    }
    uds_upgrade_reset_state();
    service_28_reset();
    service_85_reset();
    response[0] = USD_GET_POSITIVE_RSP(SID_10);
    response[1] = subfunction;
    response[2] = (uint8_t)(BOOT_UDS_P2_SERVER_MS >> 8);
    response[3] = (uint8_t)BOOT_UDS_P2_SERVER_MS;
    response[4] = (uint8_t)(BOOT_UDS_P2STAR_SERVER_MS >> 8);
    response[5] = (uint8_t)BOOT_UDS_P2STAR_SERVER_MS;
    uds_positive_rsp(response, sizeof(response));
}
