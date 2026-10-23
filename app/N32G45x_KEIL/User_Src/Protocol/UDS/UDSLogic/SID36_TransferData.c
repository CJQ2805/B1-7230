/******************************************************************************
 * UDS 0x36 TransferData
 * 每个请求包含SID、块序号和最多128字节真实固件数据；无应用层补齐字节。
 ******************************************************************************/
#include "SID36_TransferData.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "uds_upgrade.h"

bool_t service_36_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    (void)msg_buf;
    return ((msg_dlc >= 3U) &&
            (msg_dlc <= (UDS_UPGRADE_BLOCK_DATA_MAX + 2U))) ? TRUE : FALSE;
}

void service_36_TransferData(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[2];
    uint8_t block_counter;
    uint8_t result;

    if (msg_dlc < 3U || msg_dlc > (UDS_UPGRADE_BLOCK_DATA_MAX + 2U))
    {
        uds_negative_rsp(SID_36, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }

    block_counter = msg_buf[1];
    result = uds_upgrade_transfer_data(block_counter,
                                       &msg_buf[2],
                                       (uint16_t)(msg_dlc - 2U));
    if (result != UDS_UPGRADE_OK)
    {
        uds_negative_rsp(SID_36, (uds_nrc_em)result);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_36);
    rsp_buf[1] = block_counter;
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
