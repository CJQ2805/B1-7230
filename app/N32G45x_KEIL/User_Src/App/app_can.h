#ifndef __APP_CAN_H__
#define __APP_CAN_H__

#include <stdbool.h>

/*
 * APP层CAN总入口。
 *
 * app_can_init()负责底层CAN和协议模块初始化；
 * CAN_ReceiveTask()负责从d_can环形队列取帧并分发到具体协议；
 * CAN_TransmitTask()负责统一调用各协议的周期发送和应答处理。
 */
bool app_can_init(void);
void CAN_ReceiveTask(void);
void CAN_TransmitTask(void);
/* 查询CAN IAP是否占用总线，供独立协议发送出口遵守升级静默。 */
bool app_can_iap_is_busy(void);

/* 客户主动CAN会话期间阻止STOP；协议超时结束后放行现有低功耗判定。 */
bool app_can_is_sleep_allowed(void);

/* 宗申周期报文无电流超时后，低功耗模块可跳过通用60秒等待。 */
bool app_can_is_zs_idle_timeout_complete(void);

/* STOP由外部事件唤醒、CAN恢复后通知宗申协议恢复周期报文。 */
void app_can_notify_wakeup(void);

#endif
