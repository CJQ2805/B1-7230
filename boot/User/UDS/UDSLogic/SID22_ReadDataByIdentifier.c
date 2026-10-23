#include "SID22_ReadDataByIdentifier.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "app_param.h"

#include <string.h>

#define SID22_SUPPLIER_CODE_DATA_SIZE 17U

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
    uint8_t response[3U + SID22_SUPPLIER_CODE_DATA_SIZE];
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
    response[0] = USD_GET_POSITIVE_RSP(SID_22);
    response[1] = msg_buf[1];
    response[2] = msg_buf[2];

    switch (did)
    {
        case 0xF180U:
            /* Match APP's diagnostic software revision value. */
            response[3] = 'B';
            response[4] = 'V';
            response[5] = ':';
            response[6] = 0U;
            response[7] = '.';
            response[8] = 1U;
            uds_positive_rsp(response, 9U);
            break;

        case 0xF18AU:
            memset(&response[3], 0, SID22_SUPPLIER_CODE_DATA_SIZE);
            supplier_len = strlen(APP_PARAM_MANUFACTURER_NAME);
            if (supplier_len > SID22_SUPPLIER_CODE_DATA_SIZE)
            {
                uds_negative_rsp(SID_22, NRC_GENERAL_REJECT);
                break;
            }
            memcpy(&response[3], APP_PARAM_MANUFACTURER_NAME, supplier_len);
            uds_positive_rsp(response, (uint16_t)sizeof(response));
            break;

        case 0xF193U:
            /* Match APP's fixed hardware revision value. */
            response[3] = 'H';
            response[4] = 'V';
            response[5] = ':';
            response[6] = 0U;
            response[7] = '.';
            response[8] = 1U;
            uds_positive_rsp(response, 9U);
            break;

        case 0xF189U:
            if (!sid22_parse_version_component(APP_PARAM_PACKAGE_V_VERSION, 'V', &major) ||
                !sid22_parse_version_component(APP_PARAM_PACKAGE_T_VERSION, 'T', &minor))
            {
                uds_negative_rsp(SID_22, NRC_GENERAL_REJECT);
                break;
            }
            response[3] = 'S';
            response[4] = 'V';
            response[5] = ':';
            response[6] = major;
            response[7] = '.';
            response[8] = minor;
            uds_positive_rsp(response, 9U);
            break;

        default:
            uds_negative_rsp(SID_22, NRC_REQUEST_OUT_OF_RANGE);
            break;
    }
}
