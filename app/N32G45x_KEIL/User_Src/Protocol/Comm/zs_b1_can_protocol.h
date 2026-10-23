#ifndef __ZS_B1_CAN_PROTOCOL_H__
#define __ZS_B1_CAN_PROTOCOL_H__

#include <stdbool.h>

/* 初始化宗申B1协议的发送状态。 */
void zs_b1_can_protocol_init(void);

/* 由1ms CAN通信任务周期调用，更新触发窗口并调度主动报文。 */
void zs_b1_can_protocol_task(void);

/* 查询宗申客户主动发送会话是否仍要求保持MCU运行；DETD/DETC持续在位不阻止休眠。 */
bool zs_b1_can_protocol_allows_sleep(void);

/* 宗申周期报文无电流超时后停止，允许低功耗管理跳过额外空闲等待。 */
bool zs_b1_can_protocol_idle_timeout_complete(void);

/* STOP由非RTC事件唤醒后，重新启动宗申周期报文。 */
void zs_b1_can_protocol_notify_wakeup(void);

#endif
