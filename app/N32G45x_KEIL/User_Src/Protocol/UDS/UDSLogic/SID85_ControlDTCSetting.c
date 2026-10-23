/******************************************************************************
 * Customer ControlDTCSetting: 85 82 disables diagnostic DTC setting;
 * 85 81 enables it. This state never disables BMS protection or shutdown logic.
 ******************************************************************************/
#include "SID85_ControlDTCSetting.h"

#include "SID10_SessionControl.h"
#include "SID27_SecurityAccess.h"
#include "service_cfg.h"
#include "uds_service.h"

bool_t dtc_setting = UDS_DTC_SETTING_ON;

bool_t service_85_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 2U)) ? TRUE : FALSE;
}

void service_85_reset(void)
{
    dtc_setting = UDS_DTC_SETTING_ON;
}

bool_t service_85_is_enabled(void)
{
    return (dtc_setting == UDS_DTC_SETTING_ON) ? TRUE : FALSE;
}

void service_85_ControlDTCSetting(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t rsp_buf[2];

    if ((msg_buf == 0) || (msg_dlc != 2U))
    {
        uds_negative_rsp(SID_85, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }

    subfunction = (uint8_t)(msg_buf[1] & 0x7FU);
    if (subfunction == UDS_DTC_SETTING_ON)
    {
        dtc_setting = UDS_DTC_SETTING_ON;
    }
    else if (subfunction == UDS_DTC_SETTING_OFF)
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
        dtc_setting = UDS_DTC_SETTING_OFF;
    }
    else
    {
        uds_negative_rsp(SID_85, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_85);
    rsp_buf[1] = msg_buf[1];
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
