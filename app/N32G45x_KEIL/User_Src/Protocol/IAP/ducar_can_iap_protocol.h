#ifndef __DUCAR_CAN_IAP_PROTOCOL_H__
#define __DUCAR_CAN_IAP_PROTOCOL_H__

/*
 * 文件作用：
 *   DUCAR CAN IAP升级报文的协议常量定义；本文件不包含收发和升级状态机实现。
 */

#include <stdbool.h>
#include <stdint.h>

#include "Driver/d_can.h"
#include "Protocol/Comm/ducar_can_protocol.h"

#define DUCAR_CAN_IAP_CONTROL_PRIORITY      DUCAR_CAN_DEFAULT_PRIORITY
#define DUCAR_CAN_IAP_PGN                   ((uint16_t)0xDF00U)
#define DUCAR_CAN_IAP_PAGE_SIZE             ((uint32_t)128UL)
#define DUCAR_CAN_IAP_PAGE_MESSAGE_LEN      ((uint16_t)132U)
#define DUCAR_CAN_IAP_PAGE_PACKET_COUNT     ((uint8_t)19U)
#define DUCAR_CAN_IAP_PAGE_DATA_OFFSET      ((uint32_t)4UL)
#define DUCAR_CAN_IAP_PACKAGE_HEADER_SIZE   ((uint32_t)128UL)

/* 初始化APP侧CAN IAP状态机。 */
void ducar_can_iap_protocol_init(void);
/* 判断一帧是否属于APP侧CAN IAP。 */
bool ducar_can_iap_protocol_is_packet(const t_can_packet *ptCanPacket);
/* 接收并分发一帧CAN IAP报文。 */
void ducar_can_iap_protocol_receive(const t_can_packet *ptCanPacket);
/* 周期推进CAN IAP超时、应答和复位状态。 */
void ducar_can_iap_protocol_task(void);
bool ducar_can_iap_protocol_is_busy(void);

#endif
