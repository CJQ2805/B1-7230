#ifndef __BOOT_CRC_H__
#define __BOOT_CRC_H__

#include <stdint.h>

uint32_t boot_crc32(const uint8_t *data, uint32_t len);

#endif
