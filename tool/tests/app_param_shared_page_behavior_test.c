#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include "App/app_flash_map.h"
#include "App/app_param_api.h"
#include "Driver/d_flash.h"

static unsigned lock_depth;
bool d_flash_acquire(uint32_t timeout_ms) { (void)timeout_ms; ++lock_depth; return true; }
void d_flash_release(void) { assert(lock_depth != 0U); --lock_depth; }
bool d_flash_erase_page(uint32_t addr)
{
    assert(lock_depth != 0U && addr == APP_FLASH_MAP_DB_PARAM_ADDR);
    memset((void *)(uintptr_t)addr, 0xFF, APP_FLASH_MAP_FLASH_PAGE_SIZE);
    return true;
}
bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i;
    uint8_t *target = (uint8_t *)(uintptr_t)addr;
    assert(lock_depth != 0U && addr >= APP_FLASH_MAP_DB_PARAM_ADDR);
    assert(addr + len <= APP_FLASH_MAP_DB_PARAM_ADDR + APP_FLASH_MAP_FLASH_PAGE_SIZE);
    for (i = 0U; i < len; ++i) { assert(target[i] == 0xFFU); target[i] = data[i]; }
    return true;
}
int main(void)
{
    uint8_t *base = (uint8_t *)VirtualAlloc((void *)(uintptr_t)0x08030000U, 0x10000U,
                                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t before[2048];
    const uint8_t current_record[14] = {'U','C',0x40,0x42,0x0F,0,0xF0,0xFF,0xFF,0xFF,0x10,0,0,0};
    uint8_t *page;
    assert(base == (uint8_t *)(uintptr_t)0x08030000U);
    page = (uint8_t *)(uintptr_t)APP_FLASH_MAP_DB_PARAM_ADDR;
    for (uint32_t i = 0U; i < sizeof(before); ++i) page[i] = (uint8_t)((i * 31U) & 0xFFU);
    memcpy(before, page, sizeof(before));
    assert(app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET,
                                       current_record, sizeof(current_record)));
    assert(memcmp(page, current_record, sizeof(current_record)) == 0);
    assert(memcmp(page + sizeof(current_record), before + sizeof(current_record),
                  sizeof(before) - sizeof(current_record)) == 0);
    assert(lock_depth == 0U);
    assert(VirtualFree(base, 0, MEM_RELEASE));
    return 0;
}
