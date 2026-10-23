/******************************************************************************
 * Customer UDS RoutineControl implementation.
 * 0202 validates CCITT-FALSE CRC16; FF00 erases the requested APP image area;
 * FF01 checks the programming prerequisites before ECUReset 11 03.
 ******************************************************************************/
#include "SID31_RoutineControl.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "uds_upgrade.h"

static uint32_t sid31_read_u32_be(const uint8_t *data)
{
    return (((uint32_t)data[0] << 24) |
            ((uint32_t)data[1] << 16) |
            ((uint32_t)data[2] << 8) |
            (uint32_t)data[3]);
}

bool_t service_31_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint16_t rid;

    if ((msg_buf == 0) || (msg_dlc < 4U))
    {
        return FALSE;
    }

    rid = (uint16_t)(((uint16_t)msg_buf[2] << 8) | msg_buf[3]);
    if ((msg_buf[1] & 0x7FU) != 0x01U)
    {
        return (msg_dlc == 4U) ? TRUE : FALSE;
    }

    if (rid == 0x0202U)
    {
        return (msg_dlc == 6U) ? TRUE : FALSE;
    }
    if (rid == 0xFF00U)
    {
        return ((msg_dlc == 13U) && (msg_buf[4] == 0x44U)) ? TRUE : FALSE;
    }
    if (rid == 0xFF01U)
    {
        return (msg_dlc == 4U) ? TRUE : FALSE;
    }

    return (msg_dlc == 4U) ? TRUE : FALSE;
}

void service_31_RoutineControl(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[5];
    uint8_t result;
    uint8_t subfunction;
    uint16_t rid;
    uint16_t expected_crc16;
    uint32_t target_address;
    uint32_t image_size;
    bool_t crc_matches;

    (void)msg_dlc;
    subfunction = (uint8_t)(msg_buf[1] & 0x7FU);
    rid = (uint16_t)(((uint16_t)msg_buf[2] << 8) | msg_buf[3]);
    if (subfunction != 0x01U)
    {
        uds_negative_rsp(SID_31, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_31);
    rsp_buf[1] = msg_buf[1];
    rsp_buf[2] = msg_buf[2];
    rsp_buf[3] = msg_buf[3];

    if (rid == 0x0202U)
    {
        expected_crc16 = (uint16_t)(((uint16_t)msg_buf[4] << 8) | msg_buf[5]);
        result = uds_upgrade_validate_transfer_crc16(expected_crc16, &crc_matches);
        if (result != UDS_UPGRADE_OK)
        {
            uds_negative_rsp(SID_31, (uds_nrc_em)result);
            return;
        }
        rsp_buf[4] = crc_matches ? UDS_UPGRADE_ROUTINE_STATUS_OK :
                                  UDS_UPGRADE_ROUTINE_STATUS_FAIL;
        uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
        return;
    }

    if (rid == 0xFF00U)
    {
        target_address = sid31_read_u32_be(&msg_buf[5]);
        image_size = sid31_read_u32_be(&msg_buf[9]);
        result = uds_upgrade_validate_erase_request(target_address, image_size);
        if (result != UDS_UPGRADE_OK)
        {
            /* Permit only an exact retry after a previous erase completed. */
            result = uds_upgrade_erase_application(target_address, image_size);
            if (result != UDS_UPGRADE_OK)
            {
                uds_negative_rsp(SID_31, (uds_nrc_em)result);
                return;
            }
            rsp_buf[4] = UDS_UPGRADE_ROUTINE_STATUS_OK;
            uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
            return;
        }

        /* Erasing up to the APP image limit can exceed P2; report pending first. */
        uds_negative_rsp(SID_31, NRC_SERVICE_BUSY);
        result = uds_upgrade_erase_application(target_address, image_size);
        if (result == (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE)
        {
            rsp_buf[4] = UDS_UPGRADE_ROUTINE_STATUS_FAIL;
            uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
        }
        else if (result != UDS_UPGRADE_OK)
        {
            uds_negative_rsp(SID_31, (uds_nrc_em)result);
        }
        else
        {
            rsp_buf[4] = UDS_UPGRADE_ROUTINE_STATUS_OK;
            uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
        }
        return;
    }

    if (rid == 0xFF01U)
    {
        result = uds_upgrade_check_programming_dependencies();
        if (result != UDS_UPGRADE_OK)
        {
            uds_negative_rsp(SID_31, (uds_nrc_em)result);
            return;
        }
        rsp_buf[4] = UDS_UPGRADE_ROUTINE_STATUS_OK;
        uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
        return;
    }

    uds_negative_rsp(SID_31, NRC_REQUEST_OUT_OF_RANGE);
}
