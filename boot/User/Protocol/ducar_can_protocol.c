/*
 * BOOT侧DUCAR CAN公共协议工具。
 * 这里只处理扩展帧ID的编解码、合法性检查和通用帧初始化；
 * CAN硬件收发在boot_can.c，升级状态机在boot_can_upgrade.c。
 */
#include "Protocol/ducar_can_protocol.h"

/* 按优先级、保留位、页位、PF、PS、SA组装29位扩展CAN ID。 */
uint32_t ducar_can_build_id(uint8_t priority, uint8_t reserved, uint8_t data_page,
                            uint8_t pf, uint8_t ps, uint8_t sa)
{
    return (((uint32_t)priority & 0x07UL) << 26) |
           (((uint32_t)reserved & 0x01UL) << 25) |
           (((uint32_t)data_page & 0x01UL) << 24) |
           ((uint32_t)pf << 16) | ((uint32_t)ps << 8) | (uint32_t)sa;
}

/* 将29位扩展ID拆成协议字段，供BOOT升级状态机进行来源和目标判断。 */
bool ducar_can_decode_id(uint32_t id, ducar_can_id_fields_t *fields)
{
    if ((fields == 0) || ((id & ~DUCAR_CAN_EXT_ID_MASK) != 0UL))
    {
        return false;
    }

    fields->priority = (uint8_t)((id >> 26) & 0x07UL);
    fields->reserved = (uint8_t)((id >> 25) & 0x01UL);
    fields->data_page = (uint8_t)((id >> 24) & 0x01UL);
    fields->pf = (uint8_t)((id >> 16) & 0xFFUL);
    fields->ps = (uint8_t)((id >> 8) & 0xFFUL);
    fields->sa = (uint8_t)(id & 0xFFUL);
    return true;
}

/* 校验扩展ID未越界、保留位/页位正确，并限制到工程允许的优先级。 */
bool ducar_can_validate_id(uint32_t id, ducar_can_id_fields_t *fields)
{
    if (!ducar_can_decode_id(id, fields) || (fields == 0))
    {
        return false;
    }

    if ((fields->reserved != 0U) || (fields->data_page != 0U))
    {
        return false;
    }

    return ((fields->priority == DUCAR_CAN_DEFAULT_PRIORITY) ||
            (fields->priority == 3U));
}

/* 初始化BOOT协议层帧对象，默认按8字节扩展数据帧处理，空闲字节填FF。 */
void ducar_can_frame_init(ducar_can_frame_t *frame, uint32_t id)
{
    uint8_t i;

    if (frame == 0)
    {
        return;
    }

    frame->id = id & DUCAR_CAN_EXT_ID_MASK;
    frame->is_extended = 1U;
    frame->dlc = DUCAR_CAN_MAX_DATA_LEN;
    for (i = 0U; i < DUCAR_CAN_MAX_DATA_LEN; i++)
    {
        frame->data[i] = 0xFFU;
    }
}

/* 读取BOOT升级报文中的小端16位字段。 */
uint16_t ducar_can_read_u16_le(const uint8_t *data)
{
    return (data == 0) ? 0U : (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

/* 读取BOOT升级报文中的小端32位字段。 */
uint32_t ducar_can_read_u32_le(const uint8_t *data)
{
    if (data == 0)
    {
        return 0UL;
    }

    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}
