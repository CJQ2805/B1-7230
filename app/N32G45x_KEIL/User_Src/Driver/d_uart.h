#ifndef __D_UART_H__
#define __D_UART_H__

#include <stdbool.h>
#include <stdint.h>

/* 串口接收调试变量：上板时放到Watch窗口里看，不要在RX中断里打断点。 */
extern volatile uint32_t gu32_uart_rx_irq_count;
extern volatile uint32_t gu32_uart_rx_byte_count;
extern volatile uint32_t gu32_uart_rx_error_count;
extern volatile uint32_t gu32_uart_rx_overflow_count;
extern volatile uint32_t gu32_uart_tx_byte_count;
extern volatile uint32_t gu32_uart_tx_overflow_count;
extern volatile uint32_t gu32_uart_tx_complete_timeout_count;
extern volatile uint16_t gu16_uart_last_sts;
extern volatile uint8_t gu8_uart_last_rx;
extern volatile uint8_t gu8_uart_rx_debug_buf[16];
extern volatile uint8_t gu8_uart_rx_debug_pos;

/* 初始化项目通信串口，并将接收数据写入环形缓存。 */
void d_uart_init(void);
/* STOP前关闭USART2并将TX/RX切到低功耗GPIO；唤醒后调用d_uart_init恢复。 */
void d_uart_prepare_stop(void);
/* 非阻塞发送一段缓存数据，数据先进入TX环形缓存，再由TXDE中断后台发出。 */
void d_uart_send_buffer(const uint8_t* data, uint32_t length);
/* 带结果的非阻塞发送；返回false表示数据未能完整进入TX环形缓存。 */
bool d_uart_try_send_buffer(const uint8_t* data, uint32_t length);
/* 等待最后1字节真正移出串口，复位或跳转前必须调用。 */
bool d_uart_wait_tx_complete(void);
/* 返回当前环形缓存中未读取的接收字节数。 */
uint16_t d_uart_available(void);
/* 返回TX环形缓存中尚未发送完成的字节数。 */
uint16_t d_uart_tx_pending(void);
/* 读取累计接收字节数，用于判断通信是否空闲。 */
uint32_t d_uart_get_rx_count(void);
/* 读取累计发送字节数，用于判断是否还有通信活动。 */
uint32_t d_uart_get_tx_count(void);
/* 从接收环形缓存取出1字节，成功返回1。 */
uint8_t d_uart_read_byte(uint8_t* data);
/* 丢弃所有已接收但未处理的数据。 */
void d_uart_clear_rx_buffer(void);

#endif
