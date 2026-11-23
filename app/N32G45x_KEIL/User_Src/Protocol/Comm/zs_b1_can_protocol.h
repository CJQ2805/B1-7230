#ifndef __ZS_B1_CAN_PROTOCOL_H__
#define __ZS_B1_CAN_PROTOCOL_H__

#include <stdbool.h>

/*
 * 宗申B1客户侧主动报文接口，发送的均为8字节标准帧：
 * 20ms：0x089；100ms：0x019、0x621；500ms：0x419、0x619、0x519、0x5A0、0x5AB。
 * DETD(IGN)上升沿或DETC插入/拔出后，0x419先发送11帧Data0=0x02 Wakeup，再周期发送Data0=0x00 Active。
 * KEY缺失且业务电流为零时，从20秒停发等待窗口开始，0x419按500ms周期发送Data0=0x01 Sleep；等待期间其他周期报文仍发送。
 * 连续空闲满20秒后停止其他周期报文；确认Sleep帧已提交后结束主动发送，允许低功耗流程继续判断STOP条件。
 * 唤醒时按500ms周期发送11帧Data0=0x02 Wakeup，成功发送满11帧后再恢复Data0=0x00 Active。
 * 0x105 KEY心跳接收及3秒有效期由输入信号模块维护，此模块只读取其结果。
 * KEY在位时保持发送并清零停发计时；KEY移除后继续发送至少配置时长。
 * KEY移除期间若仍有电流，则等电流归零后再累计配置时长，然后停止发送。
 */

/* 初始化发送计时器、报文计数器和充电状态机。 */
void zs_b1_can_protocol_init(void);

/*
 * 由CAN通信任务周期调用：读取BMS快照、更新停充状态和主动发送调度。
 * 普通CAN的TP传输活跃时保持或恢复发送会话，避免日志/长帧传输被空闲静默打断。
 * 发送周期与超时使用TIM6实际毫秒差值累计；任务调度和CAN邮箱仍可能带来少量延迟。
 */
void zs_b1_can_protocol_task(bool can_transport_active);

/*
 * true：宗申周期发送已经停止并锁存空闲；20秒Sleep准备期仍在发周期报文，不属于静默。
 * 并联模块只在该状态保持对端缓存和已分配地址，避免真正静默时触发离线重判。
 */
bool zs_b1_can_protocol_is_silent(void);

/* true：宗申主动发送会话已结束；最终是否STOP仍由低功耗门禁决定。 */
bool zs_b1_can_protocol_allows_sleep(void);

/* true：电流空闲超时且主动发送已停止，低功耗管理器据此判断当前会话状态。 */
bool zs_b1_can_protocol_idle_timeout_complete(void);

/* STOP由非RTC事件唤醒后调用，重新启动宗申周期发送会话。 */
void zs_b1_can_protocol_notify_wakeup(void);

#endif
