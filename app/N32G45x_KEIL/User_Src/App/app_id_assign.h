#ifndef __APP_ID_ASSIGN_H__
#define __APP_ID_ASSIGN_H__

#include <stdbool.h>
#include <stdint.h>

#include "Driver/d_can.h"

/* UID编址广播使用的固定扩展帧ID；APP CAN调度仅在并联配置启用后分发。 */
#define ID_ASSIGN_MSG                 ((uint32_t)0x18F10815UL)
/* 最多15包：本机1包加14个对端，地址按0x16+序号连续分配至0x24，包含0x1E。 */
#define APP_ID_ASSIGN_MAX_PACK_COUNT  ((uint8_t)15U)
#define APP_ID_ASSIGN_PEER_CAPACITY   ((uint8_t)(APP_ID_ASSIGN_MAX_PACK_COUNT - 1U))

typedef enum
{
    ID_ASSIGN_IDLE = 0U, /* 空闲；下一次task(enable=true)会清表、读取UID并启动广播。 */
    ID_ASSIGN_MSG_SEND,  /* 编址收集窗；按50ms周期广播本机UID并收集不同对端UID。 */
    ID_ASSIGNING,        /* 收集窗结束；按UID升序计算本机rank。 */
    ID_ASSIGNED,         /* rank已确定；周期复核在线包数并等待成员变化。 */
    ID_MAX
} ID_ASSIGN_E;

/* 通信任务推进/更新，低功耗任务读取当前编址阶段以决定是否允许进入休眠。 */
extern volatile ID_ASSIGN_E id_assign_state;

/* 接收UID编址帧；由CAN通信任务在编址窗口内调用并更新对端UID集合。 */
void app_id_assign_msg_handle(const t_can_packet *can_packet);

/* 静默期间收到UID帧时，立即开始新编址并把触发帧作为本轮首个对端UID处理。 */
void app_id_assign_restart_with_frame(const t_can_packet *can_packet);

/*
 * 推进编址状态机；enable表示本轮允许编址，parallel_num为状态报文缓存得到的在线总包数。
 * parallel_num为0时不因在线数暂不可用触发成员数重排；非0时用于核对UID收集到的总包数。
 * 返回本机编址序号；后续接入时CAN地址=DUCAR_CAN_BMS_A_ADDRESS+序号，
 * 序号0..14对应地址0x16..0x24，中间不跳过0x1E。
 */
uint8_t app_id_assign_task(bool enable, uint8_t parallel_num);

#endif
