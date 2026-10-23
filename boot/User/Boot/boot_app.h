#ifndef __BOOT_APP_H__
#define __BOOT_APP_H__

#include <stdbool.h>
#include <stdint.h>

bool boot_app_app1_is_valid(void);
bool boot_app_backup_is_valid(void);
bool boot_app_jump_app1(void);

extern volatile uint32_t g_boot_app_jump_base;
extern volatile uint32_t g_boot_app_jump_stack;
extern volatile uint32_t g_boot_app_jump_reset;
extern volatile uint32_t g_boot_app_jump_vtor;
extern volatile uint32_t g_boot_app_jump_stage;

#endif
