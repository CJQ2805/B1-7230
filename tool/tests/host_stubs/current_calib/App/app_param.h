#ifndef TEST_APP_PARAM_H
#define TEST_APP_PARAM_H

#include <stdbool.h>
#include <stdint.h>

#define APP_PARAM_CURRENT_ZERO_DEADBAND_MA 300

bool app_param_flash_page_update(uint32_t offset, const uint8_t *data, uint32_t len);

#endif
