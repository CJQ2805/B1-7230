/*
 * 文件作用：
 *   Ducar串口升级协议的工程本地帧解析和组帧模块，供当前工程的升级管理模块使用。
 *
 * 主要用途：
 *   1. 按升级协议逐字节解析SOF、地址、CMD、LEN、DATA、校验和帧尾。
 *   2. 生成升级命令回复帧；APP和BOOT各自维护一份同协议实现。
 *   3. 解析固件头包中的品号、版本、长度、CRC等升级校验信息。
 *
 * 适用范围：
 *   适用于Ducar当前串口IAP升级协议。该文件只处理协议字节格式，不负责串口收发、不负责Flash写入、
 *   不负责BOOT和APP跳转策略。
 *
 * 调用关系：
 *   BOOT的boot_protocol模块和APP的app_iap_manage模块调用本文件接口；串口驱动只负责提供原始字节。
 *
 * 注意事项：
 *   修改帧格式时必须同步更新BOOT版本、APP版本、打包工具和协议文档示例，避免上位机与设备端解析不一致。
 */
#include "Protocol/ducar_iap_protocol.h"

#include <string.h>

typedef enum
{
    DUCAR_IAP_STATE_WAIT_SOF = 0U, /* 等待帧头0xAA。 */
    DUCAR_IAP_STATE_ADDR,          /* 接收地址0x01。 */
    DUCAR_IAP_STATE_CMD,           /* 接收命令字。 */
    DUCAR_IAP_STATE_LEN,           /* 接收DATA长度。 */
    DUCAR_IAP_STATE_DATA,          /* 接收DATA内容。 */
    DUCAR_IAP_STATE_SUM,           /* 接收单字节累加校验和。 */
    DUCAR_IAP_STATE_EOF0,          /* 接收帧尾0x3A。 */
    DUCAR_IAP_STATE_EOF1           /* 接收帧尾0x4B。 */
} ducar_iap_state_t;

static uint16_t ducar_iap_read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t ducar_iap_read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0]) |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void ducar_iap_parser_reset(ducar_iap_parser_t *parser)
{
    parser->state = (uint8_t)DUCAR_IAP_STATE_WAIT_SOF;
    parser->data_index = 0U;
    parser->checksum = 0U;
    parser->frame.cmd = 0U;
    parser->frame.len = 0U;
}

static ducar_iap_parse_result_t ducar_iap_parser_error(ducar_iap_parser_t *parser, ducar_iap_frame_t *out_frame)
{
    if (out_frame != 0)
    {
        *out_frame = parser->frame; /* 保留命令字，方便上层按原命令回复NAK。 */
    }

    ducar_iap_parser_reset(parser);
    return DUCAR_IAP_PARSE_ERROR;
}

void ducar_iap_parser_init(ducar_iap_parser_t *parser)
{
    if (parser == 0)
    {
        return;
    }

    ducar_iap_parser_reset(parser);
}

ducar_iap_parse_result_t ducar_iap_parser_input(ducar_iap_parser_t *parser, uint8_t byte, ducar_iap_frame_t *out_frame)
{
    if (parser == 0)
    {
        return DUCAR_IAP_PARSE_NONE;
    }

    switch ((ducar_iap_state_t)parser->state)
    {
        case DUCAR_IAP_STATE_WAIT_SOF:
            if (byte == DUCAR_IAP_SOF)
            {
                ducar_iap_parser_reset(parser);
                parser->checksum = DUCAR_IAP_SOF;
                parser->state = (uint8_t)DUCAR_IAP_STATE_ADDR;
            }
            break;

        case DUCAR_IAP_STATE_ADDR:
            if (byte != DUCAR_IAP_ADDR)
            {
                ducar_iap_parser_reset(parser);
                if (byte == DUCAR_IAP_SOF)
                {
                    parser->checksum = DUCAR_IAP_SOF;
                    parser->state = (uint8_t)DUCAR_IAP_STATE_ADDR;
                }
                break;
            }

            parser->checksum = (uint8_t)(DUCAR_IAP_SOF + DUCAR_IAP_ADDR);
            parser->state = (uint8_t)DUCAR_IAP_STATE_CMD;
            break;

        case DUCAR_IAP_STATE_CMD:
            parser->frame.cmd = byte;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            parser->state = (uint8_t)DUCAR_IAP_STATE_LEN;
            break;

        case DUCAR_IAP_STATE_LEN:
            parser->frame.len = byte;
            parser->data_index = 0U;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->frame.len == 0U)
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_SUM;
            }
            else
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_DATA;
            }
            break;

        case DUCAR_IAP_STATE_DATA:
            parser->frame.data[parser->data_index] = byte;
            parser->data_index++;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->data_index >= parser->frame.len)
            {
                parser->state = (uint8_t)DUCAR_IAP_STATE_SUM;
            }
            break;

        case DUCAR_IAP_STATE_SUM:
            if (byte != parser->checksum)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_IAP_STATE_EOF0;
            break;

        case DUCAR_IAP_STATE_EOF0:
            if (byte != DUCAR_IAP_EOF0)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_IAP_STATE_EOF1;
            break;

        case DUCAR_IAP_STATE_EOF1:
            if (byte != DUCAR_IAP_EOF1)
            {
                return ducar_iap_parser_error(parser, out_frame);
            }

            if (out_frame != 0)
            {
                *out_frame = parser->frame;
            }
            ducar_iap_parser_reset(parser);
            return DUCAR_IAP_PARSE_OK;

        default:
            ducar_iap_parser_reset(parser);
            break;
    }

    return DUCAR_IAP_PARSE_NONE;
}

uint16_t ducar_iap_pack_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out_frame, uint16_t out_size)
{
    uint8_t checksum;
    uint16_t index;
    uint8_t i;

    if ((out_frame == 0) || (out_size < (uint16_t)(7U + len)) || ((data == 0) && (len > 0U)))
    {
        return 0U;
    }

    index = 0U;
    checksum = (uint8_t)(DUCAR_IAP_SOF + DUCAR_IAP_ADDR);

    out_frame[index++] = DUCAR_IAP_SOF;
    out_frame[index++] = DUCAR_IAP_ADDR;

    out_frame[index++] = cmd;
    checksum = (uint8_t)(checksum + cmd);

    out_frame[index++] = len;
    checksum = (uint8_t)(checksum + len);

    for (i = 0U; i < len; i++)
    {
        out_frame[index++] = data[i];
        checksum = (uint8_t)(checksum + data[i]);
    }

    out_frame[index++] = checksum;
    out_frame[index++] = DUCAR_IAP_EOF0;
    out_frame[index++] = DUCAR_IAP_EOF1;

    return index;
}

uint16_t ducar_iap_get_frame_no(const ducar_iap_frame_t *frame)
{
    if ((frame == 0) || (frame->len < 2U))
    {
        return 0xFFFFU;
    }

    return ducar_iap_read_le16(&frame->data[0]);
}

bool ducar_iap_parse_update_start(const ducar_iap_frame_t *frame, ducar_iap_update_info_t *info)
{
    if ((frame == 0) || (info == 0) || (frame->cmd != DUCAR_IAP_CMD_UPDATE_START) || (frame->len != 4U))
    {
        return false;
    }

    info->page_count = ducar_iap_read_le16(&frame->data[0]);
    info->page_size = ducar_iap_read_le16(&frame->data[2]);
    return true;
}

bool ducar_iap_parse_firmware_info(const ducar_iap_frame_t *frame, ducar_iap_firmware_info_t *info)
{
    const uint8_t mark[DUCAR_IAP_META_MARK_LEN] = {'D', 'U', 'C', 'A', 'R', 0xFFU, 0xFFU};
    const uint8_t *meta;

    if ((frame == 0) || (info == 0) || (frame->cmd != DUCAR_IAP_CMD_PROGRAM) ||
        (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN) || (ducar_iap_get_frame_no(frame) != 0U))
    {
        return false;
    }

    meta = &frame->data[2];
    if (memcmp(&meta[DUCAR_IAP_META_MARK_OFFSET], mark, DUCAR_IAP_META_MARK_LEN) != 0)
    {
        return false;
    }

    memset(info, 0, sizeof(*info));
    memcpy(info->part_number, &meta[DUCAR_IAP_META_PART_OFFSET], DUCAR_IAP_META_PART_LEN);
    info->part_number[DUCAR_IAP_META_PART_LEN] = '\0';
    info->crc32 = ducar_iap_read_le32(&meta[DUCAR_IAP_META_CRC_OFFSET]);
    info->size = ducar_iap_read_le32(&meta[DUCAR_IAP_META_SIZE_OFFSET]);
    return true;
}

bool ducar_iap_part_number_is_allowed(const char *local_part_number, const char *firmware_part_number)
{
    if ((local_part_number == 0) || (firmware_part_number == 0))
    {
        return false;
    }

    /*
     * A000000000是未定义品号时的强制升级通配品号。
     * 上位机使用该品号出包时，BOOT和APP都允许继续升级。
     */
    if (strncmp(firmware_part_number, DUCAR_IAP_DEFAULT_PART_NUMBER, DUCAR_IAP_META_PART_LEN) == 0)
    {
        return true;
    }

    return (strncmp(firmware_part_number, local_part_number, DUCAR_IAP_META_PART_LEN) == 0);
}

uint16_t ducar_iap_calc_data_frames(uint32_t firmware_size)
{
    if (firmware_size == 0UL)
    {
        return 0U;
    }

    return (uint16_t)((firmware_size + (uint32_t)DUCAR_IAP_PAGE_SIZE - 1UL) / (uint32_t)DUCAR_IAP_PAGE_SIZE);
}

uint32_t ducar_iap_crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    uint8_t bit;

    if ((data == 0) && (len > 0UL))
    {
        return 0UL;
    }

    for (i = 0UL; i < len; i++)
    {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; bit++)
        {
            if ((crc & 1UL) != 0UL)
            {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return ~crc;
}
