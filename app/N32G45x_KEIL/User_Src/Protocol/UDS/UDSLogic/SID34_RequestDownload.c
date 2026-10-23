/******************************************************************************
 * UDS 0x34 RequestDownload
 * 请求格式：34 00 44 [地址4字节大端] [本次S-record数据长度4字节大端]。
 * ZCANPRO将裸APP S19的记录数据写到其地址，当前升级包目标是APP1。
 ******************************************************************************/
#include "SID34_RequestDownload.h"

#include "service_cfg.h"
#include "uds_service.h"
#include "uds_upgrade.h"

static uint32_t sid34_read_u32_be(const uint8_t *data)
{
    return (((uint32_t)data[0] << 24) |
            ((uint32_t)data[1] << 16) |
            ((uint32_t)data[2] << 8) |
            (uint32_t)data[3]);
}

bool_t service_34_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    (void)msg_buf;
    return (msg_dlc == 11U) ? TRUE : FALSE;
}

void service_34_RequestDownload(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t rsp_buf[4];
    uint32_t target_address;
    uint32_t image_size;
    uint8_t result;

    if (msg_dlc != 11U || msg_buf[1] != 0x00U || msg_buf[2] != 0x44U)
    {
        uds_negative_rsp(SID_34, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
        return;
    }

    target_address = sid34_read_u32_be(&msg_buf[3]);
    image_size = sid34_read_u32_be(&msg_buf[7]);
    result = uds_upgrade_request_download(target_address, image_size);
    if (result != UDS_UPGRADE_OK)
    {
        uds_negative_rsp(SID_34, (uds_nrc_em)result);
        return;
    }

    /* 0x0082表示最大UDS传输块总长130字节：SID、BSC和128字节固件数据。 */
    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_34);
    rsp_buf[1] = 0x20U; /* lengthFormatIdentifier：后续maxNumberOfBlockLength为2字节。 */
    rsp_buf[2] = 0x00U;
    rsp_buf[3] = 0x82U;
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
