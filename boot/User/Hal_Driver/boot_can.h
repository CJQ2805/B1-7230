/* BOOT CAN底层初始化、收发队列、错误统计和中断接口。 */
#ifndef __BOOT_CAN_H__
#define __BOOT_CAN_H__

#include <stdbool.h>
#include <stdint.h>
#include "Protocol/ducar_can_protocol.h"

#define BOOT_CAN_BAUDRATE ((uint32_t)500000UL)

/* 初始化CAN GPIO、CAN1外设、过滤器、中断和BOOT升级状态机。 */
void boot_can_init(void);
/* 关闭CAN中断和CAN1外设。 */
void boot_can_deinit(void);
/* 维护错误状态、发送邮箱/队列和BOOT升级接收队列。 */
void boot_can_task(void);
/* 提交一帧BOOT扩展数据帧，必要时进入发送环形队列。 */
bool boot_can_send_frame(const ducar_can_frame_t *frame);
/* CAN RX0中断服务函数。 */
void boot_can_irq_handler(void);
/* CAN状态/错误中断服务函数。 */
void boot_can_error_irq_handler(void);
/* 查询软件队列及硬件邮箱是否均已发送完成。 */
bool boot_can_is_tx_idle(void);
/* 以下接口返回CAN运行统计，供升级状态机和调试使用。 */
uint32_t boot_can_get_tx_failed_count(void);
uint32_t boot_can_get_rx_overflow_count(void);
uint32_t boot_can_get_hw_fifo_overrun_count(void);
uint32_t boot_can_get_tx_queue_overflow_count(void);
uint32_t boot_can_get_error_warning_count(void);
uint32_t boot_can_get_error_passive_count(void);
uint32_t boot_can_get_bus_off_count(void);
uint32_t boot_can_get_bus_off_recovery_count(void);
uint8_t boot_can_get_last_error_code(void);

#endif
