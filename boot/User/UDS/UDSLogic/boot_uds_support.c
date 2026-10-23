#include "SID28_CommunicationControl.h"
#include "SID10_SessionControl.h"
#include "SID27_SecurityAccess.h"
#include "SID85_ControlDTCSetting.h"
#include "service_cfg.h"
#include "uds_service.h"

static bool_t s_dtc_setting_enabled = TRUE;

bool_t service_28_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 3U)) ? TRUE : FALSE;
}

void service_28_CommunicationControl(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t response[2];

    if ((msg_buf == 0) || (msg_dlc != 3U) || (msg_buf[2] != 0x03U))
    {
        uds_negative_rsp(SID_28, NRC_REQUEST_OUT_OF_RANGE);
        return;
    }
    subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);
    if (subfunction == 0x03U)
    {
        if (get_current_session() != UDS_SESSION_EXT)
        {
            uds_negative_rsp(SID_28, NRC_SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION);
            return;
        }
        if (get_current_sa_lv() < UDS_SA_LV1)
        {
            uds_negative_rsp(SID_28, NRC_SECURITY_ACCESS_DENIED);
            return;
        }
        /* No APP communication runs while BOOT waits for an image. */
    }
    else if (subfunction != 0x00U)
    {
        uds_negative_rsp(SID_28, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    response[0] = USD_GET_POSITIVE_RSP(SID_28);
    response[1] = msg_buf[1];
    uds_positive_rsp(response, sizeof(response));
}

/* BOOT does not run application DUCAR communications or DTC logging controls. */
void service_28_reset(void)
{
}

void service_28_timeout_task_1ms(void)
{
}

void service_28_on_s3_timeout(void)
{
    service_28_reset();
}

void service_85_reset(void)
{
    s_dtc_setting_enabled = TRUE;
}

bool_t service_85_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 2U)) ? TRUE : FALSE;
}

bool_t service_85_is_enabled(void)
{
    return s_dtc_setting_enabled;
}

void service_85_ControlDTCSetting(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t response[2];

    if ((msg_buf == 0) || (msg_dlc != 2U))
    {
        uds_negative_rsp(SID_85, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }
    subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);
    if (subfunction == 0x01U)
    {
        s_dtc_setting_enabled = TRUE;
    }
    else if (subfunction == 0x02U)
    {
        if (get_current_session() != UDS_SESSION_EXT)
        {
            uds_negative_rsp(SID_85, NRC_SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION);
            return;
        }
        if (get_current_sa_lv() < UDS_SA_LV1)
        {
            uds_negative_rsp(SID_85, NRC_SECURITY_ACCESS_DENIED);
            return;
        }
        s_dtc_setting_enabled = FALSE;
    }
    else
    {
        uds_negative_rsp(SID_85, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    response[0] = USD_GET_POSITIVE_RSP(SID_85);
    response[1] = msg_buf[1];
    uds_positive_rsp(response, sizeof(response));
}
