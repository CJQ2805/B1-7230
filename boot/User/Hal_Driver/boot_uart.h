#ifndef __BOOT_UART_H__
#define __BOOT_UART_H__

#include <stdbool.h>
#include <stdint.h>

void boot_uart_init(void);
void boot_uart_send_byte(uint8_t data);
void boot_uart_send_buffer(const uint8_t *data, uint32_t len);
void boot_uart_wait_tx_complete(void);
bool boot_uart_read_byte(uint8_t *data);

#endif
