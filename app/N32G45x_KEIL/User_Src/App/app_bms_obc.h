#ifndef __APP_BMS_OBC_H__
#define __APP_BMS_OBC_H__

#include <stdint.h>

#include "App/app_bms_types.h"

#define APP_BMS_OBC_RX_CAN_ID ((uint32_t)0x393U)

/* 初始化OBC接收状态；上电默认“不在位”。 */
void app_bms_obc_init(void);

/* 接收并保存一帧OBC故障报文；仅接受完整8字节报文。 */
void app_bms_obc_receive(const uint8_t *data, uint8_t length);

/* 由10ms数据任务调用，结合已去抖DETC输入和接收超时刷新OBC状态。 */
void app_bms_obc_task(uint8_t detc_present);

/* 复制读取最近一帧和当前OBC状态。 */
void app_bms_obc_read(app_bms_obc_state_t *state);

#endif
