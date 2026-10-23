#ifndef __BOOT_FLASH_H__
#define __BOOT_FLASH_H__

#include <stdbool.h>
#include <stdint.h>

bool boot_flash_erase_page(uint32_t page_addr);
bool boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);
bool boot_flash_read(uint32_t addr, uint8_t *data, uint32_t len);
bool boot_flash_is_blank(uint32_t addr, uint32_t len);

#endif
