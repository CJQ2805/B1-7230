/******************************************************************************
 * Customer UDS 0x22 ReadDataByIdentifier.
 * F189 deliberately keeps the previously agreed three-byte V/T/HW version.
 ******************************************************************************/
#include "SID22_ReadDataByIdentifier.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "App/app_param.h"
#include "App/app_param_api.h"

#include <string.h>

#define SID22_SUPPLIER_CODE_DATA_SIZE 17U

/* Convert a Vnn/Tn setting to the numeric byte carried in the customer DID. */
static bool_t sid22_parse_version_component(const char *version,
                                            char prefix,
                                            uint8_t *value_out)
{
    uint16_t value = 0U;
    uint8_t digits = 0U;
    const char *cursor;

    if ((version == 0) || (value_out == 0) || (version[0] != prefix))
    {
        return FALSE;
    }
    for (cursor = &version[1]; *cursor != '\0'; ++cursor)
    {
        if ((*cursor < '0') || (*cursor > '9'))
        {
            return FALSE;
        }
        value = (uint16_t)((value * 10U) + (uint16_t)(*cursor - '0'));
        if (value > 0xFFU)
        {
            return FALSE;
        }
        ++digits;
    }
    if (digits == 0U)
    {
        return FALSE;
    }
    *value_out = (uint8_t)value;
    return TRUE;
}

bool_t service_22_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 3U)) ? TRUE : FALSE;
}

void service_22_ReadDataByIdentifier(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[3U + SID22_SUPPLIER_CODE_DATA_SIZE];
    uint16_t did;
    uint8_t major;
    uint8_t minor;
    size_t supplier_len;

    if ((msg_buf == 0) || (msg_dlc != 3U))
    {
        uds_negative_rsp(SID_22, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }
    did = (uint16_t)(((uint16_t)msg_buf[1] << 8) | msg_buf[2]);
    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_22);
    rsp_buf[1] = msg_buf[1];
    rsp_buf[2] = msg_buf[2];

    switch (did)
    {
        case 0xF180U:
            /* Fixed boot-software revision bytes 00.01, encoded as "BV:x.y". */
            rsp_buf[3] = 'B';
            rsp_buf[4] = 'V';
            rsp_buf[5] = ':';
            rsp_buf[6] = 0U;
            rsp_buf[7] = '.';
            rsp_buf[8] = 1U;
            uds_positive_rsp(rsp_buf, 9U);
            break;

        case 0xF18AU:
            /* Supplier code is ASCII, fixed to 17 bytes with 00 tail padding. */
            memset(&rsp_buf[3], 0, SID22_SUPPLIER_CODE_DATA_SIZE);
            supplier_len = strlen(APP_PARAM_MANUFACTURER_NAME);
            if (supplier_len > SID22_SUPPLIER_CODE_DATA_SIZE)
            {
                uds_negative_rsp(SID_22, NRC_GENERAL_REJECT);
                break;
            }
            memcpy(&rsp_buf[3], APP_PARAM_MANUFACTURER_NAME, supplier_len);
            uds_positive_rsp(rsp_buf, (uint16_t)sizeof(rsp_buf));
            break;

        case 0xF193U:
            /* Fixed hardware revision, matching F180: 00.01. */
            rsp_buf[3] = 'H';
            rsp_buf[4] = 'V';
            rsp_buf[5] = ':';
            rsp_buf[6] = 0U;
            rsp_buf[7] = '.';
            rsp_buf[8] = 1U;
            uds_positive_rsp(rsp_buf, 9U);
            break;

        case 0xF189U:
            if (!sid22_parse_version_component(app_param_get_package_v_version(), 'V', &major) ||
                !sid22_parse_version_component(app_param_get_package_t_version(), 'T', &minor))
            {
                uds_negative_rsp(SID_22, NRC_GENERAL_REJECT);
                break;
            }
            /* Customer F189 DataRecord: "SV:" + software high byte + '.' + low byte. */
            rsp_buf[3] = 'S';
            rsp_buf[4] = 'V';
            rsp_buf[5] = ':';
            rsp_buf[6] = major;
            rsp_buf[7] = '.';
            rsp_buf[8] = minor;
            uds_positive_rsp(rsp_buf, 9U);
            break;

        default:
            uds_negative_rsp(SID_22, NRC_REQUEST_OUT_OF_RANGE);
            break;
    }
}
