/******************************************************************************
 * Customer UDS 0x2E WriteDataByIdentifier.
 * F184 stores the eight-byte programming fingerprint to APP database Page3.
 ******************************************************************************/
#include "SID2E_WriteDataByIdentifier.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "uds_upgrade.h"
#include "uds_customer_profile.h"

bool_t service_2E_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint16_t did;

    if ((msg_buf == 0) || (msg_dlc < 3U))
    {
        return FALSE;
    }

    did = (uint16_t)(((uint16_t)msg_buf[1] << 8) | msg_buf[2]);
    if (did == 0xF184U)
    {
        return (msg_dlc == (3U + UDS_CUSTOMER_FINGERPRINT_SIZE)) ? TRUE : FALSE;
    }
    return TRUE;
}

void service_2E_WriteDataByIdentifier(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[3];
    uint16_t did;
    uint8_t result;

    if ((msg_buf == 0) || (msg_dlc < 3U))
    {
        uds_negative_rsp(SID_2E, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }

    did = (uint16_t)(((uint16_t)msg_buf[1] << 8) | msg_buf[2]);
    if ((did != 0xF184U) ||
        (msg_dlc != (3U + UDS_CUSTOMER_FINGERPRINT_SIZE)))
    {
        uds_negative_rsp(SID_2E, NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    result = uds_upgrade_store_fingerprint(&msg_buf[3],
                                          UDS_CUSTOMER_FINGERPRINT_SIZE);
    if (result != UDS_UPGRADE_OK)
    {
        uds_negative_rsp(SID_2E, (uds_nrc_em)result);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_2E);
    rsp_buf[1] = msg_buf[1];
    rsp_buf[2] = msg_buf[2];
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
