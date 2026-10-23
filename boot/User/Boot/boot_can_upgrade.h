/* BOOT侧CAN升级状态机的初始化、周期和报文入口。 */
#ifndef __BOOT_CAN_UPGRADE_H__
#define __BOOT_CAN_UPGRADE_H__

#include <stdbool.h>
#include "Protocol/ducar_can_protocol.h"

/* 升级状态机通过该回调把ACK/CTS/EOM/ABORT交给底层CAN发送。 */
typedef bool (*boot_can_upgrade_tx_callback_t)(const ducar_can_frame_t *frame);

/* 清除升级会话并登记底层CAN发送回调。 */
void boot_can_upgrade_init(boot_can_upgrade_tx_callback_t tx_callback);
/* 周期推进接收超时、搬运准备和复位动作。 */
void boot_can_upgrade_task(void);
/* 接收一帧CAN报文并按PF分发到BOOT升级状态机。 */
bool boot_can_upgrade_on_receive(const ducar_can_frame_t *frame);

#endif
