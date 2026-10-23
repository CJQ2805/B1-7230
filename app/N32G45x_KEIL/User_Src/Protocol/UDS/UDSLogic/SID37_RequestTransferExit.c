/******************************************************************************
 * UDS 0x37 RequestTransferExit
 * 客户格式只有SID，不携带CRC；整次下载的CRC16由RoutineControl单独校验。
 ******************************************************************************/
#include "SID37_RequestTransferExit.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "uds_upgrade.h"

bool_t service_37_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    (void)msg_buf;
    return ((msg_buf != 0) && (msg_dlc == 1U)) ? TRUE : FALSE;
}

void service_37_RequestTransferExit(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[1];
    uint8_t result;

    (void)msg_buf;
    if (msg_dlc != 1U)
    {
        uds_negative_rsp(SID_37, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }

    result = uds_upgrade_transfer_exit();
    if (result != UDS_UPGRADE_OK)
    {
        uds_negative_rsp(SID_37, (uds_nrc_em)result);
        return;
    }

    /* 0x77只确认数据传输结束且整包CRC校验通过；此时尚未跳转BOOT。 */
    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_37);
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
