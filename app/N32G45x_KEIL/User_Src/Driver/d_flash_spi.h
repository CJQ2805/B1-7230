#ifndef _D_FLASH_SPI_H_
#define _D_FLASH_SPI_H_

#include <stdbool.h>
#include <stdint.h>

/* 外部FLASH硬件接口：SPI2，PB12为软件片选，SPI模式0，8位数据。 */
void d_flash_spi_init(void);
void d_flash_spi_select(void);
void d_flash_spi_deselect(void);
bool d_flash_spi_transfer_byte(uint8_t tx_data, uint8_t *rx_data);
bool d_flash_spi_transfer(const uint8_t *tx_data, uint8_t *rx_data, uint16_t length);

#endif
