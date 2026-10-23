/* BOOT侧DUCAR CAN扩展帧公共定义和ID/小端字段工具声明。 */
#ifndef __DUCAR_CAN_PROTOCOL_H__
#define __DUCAR_CAN_PROTOCOL_H__

#include <stdbool.h>
#include <stdint.h>

#define DUCAR_CAN_MAX_DATA_LEN          ((uint8_t)8U)
#define DUCAR_CAN_EXT_ID_MASK           ((uint32_t)0x1FFFFFFFUL)
#define DUCAR_CAN_DEFAULT_PRIORITY      ((uint8_t)6U)
#define DUCAR_CAN_BMS_A_ADDRESS         ((uint8_t)0x16U)
#define DUCAR_CAN_PC_ADDRESS            ((uint8_t)0x3AU)
#define DUCAR_CAN_GLOBAL_ADDRESS        ((uint8_t)0xFFU)

#define DUCAR_CAN_PF_ACK                ((uint8_t)0xE8U)
#define DUCAR_CAN_PF_REQUEST            ((uint8_t)0xEAU)
#define DUCAR_CAN_PF_TP_CM              ((uint8_t)0xECU)
#define DUCAR_CAN_PF_TP_DP              ((uint8_t)0xEBU)
#define DUCAR_CAN_PF_BOOT_CONTROL       ((uint8_t)0xE1U)
#define DUCAR_CAN_PGN_F200              ((uint16_t)0xF200U)
#define DUCAR_CAN_ACK                   ((uint8_t)0x00U)
#define DUCAR_CAN_NAK                   ((uint8_t)0x01U)

typedef struct
{
    uint32_t id;
    uint8_t is_extended;
    uint8_t dlc;
    uint8_t data[DUCAR_CAN_MAX_DATA_LEN];
} ducar_can_frame_t;

typedef struct
{
    uint8_t priority;
    uint8_t reserved;
    uint8_t data_page;
    uint8_t pf;
    uint8_t ps;
    uint8_t sa;
} ducar_can_id_fields_t;

/* 组装29位扩展ID。 */
uint32_t ducar_can_build_id(uint8_t priority, uint8_t reserved, uint8_t data_page,
                            uint8_t pf, uint8_t ps, uint8_t sa);
/* 拆分29位扩展ID。 */
bool ducar_can_decode_id(uint32_t id, ducar_can_id_fields_t *fields);
/* 检查ID字段是否满足BOOT协议约束。 */
bool ducar_can_validate_id(uint32_t id, ducar_can_id_fields_t *fields);
/* 初始化一帧默认扩展数据帧。 */
void ducar_can_frame_init(ducar_can_frame_t *frame, uint32_t id);
/* 读取小端16位/32位字段。 */
uint16_t ducar_can_read_u16_le(const uint8_t *data);
uint32_t ducar_can_read_u32_le(const uint8_t *data);

#endif
