#include <stdbool.h>
#include <stdint.h>

#include "App/app_flash_map.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "HSI_afe/hsi_afe.h"

uint8_t g_test_flash[2048];
hsi_afe_t gt_hsi_afe;

void app_current_calib_test_critical_enter(void) {}
void app_current_calib_test_critical_exit(void) {}

const uint8_t *app_current_calib_test_flash_ptr(uint32_t addr)
{
    (void)addr;
    return g_test_flash;
}
bool d_flash_acquire(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void d_flash_release(void) {}
bool d_flash_is_blank(uint32_t addr, uint32_t len) { (void)addr; (void)len; return true; }
bool d_flash_erase_page(uint32_t addr) { (void)addr; return true; }
bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{ (void)addr; (void)data; (void)len; return true; }

bool app_param_flash_page_update(uint32_t offset, const uint8_t *data, uint32_t len)
{
    (void)offset;
    (void)data;
    (void)len;
    return true;
}
