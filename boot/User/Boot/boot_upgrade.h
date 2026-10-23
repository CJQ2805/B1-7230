#ifndef __BOOT_UPGRADE_H__
#define __BOOT_UPGRADE_H__

#include <stdbool.h>
#include <stdint.h>

uint32_t boot_upgrade_get_flag(void);
bool boot_upgrade_set_flag(uint32_t flag);
bool boot_upgrade_prepare_move(uint32_t image_size, uint32_t image_crc32);
bool boot_upgrade_prepare_app1(uint32_t image_size, uint32_t image_crc32);
bool boot_upgrade_get_move_info(uint32_t *image_size, uint32_t *image_crc32);
bool boot_upgrade_app1_ready_is_valid(void);
bool boot_upgrade_move_backup_to_app1(void);

#endif
