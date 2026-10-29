#ifndef TEST_LED_APP_BMS_SNAPSHOT_H
#define TEST_LED_APP_BMS_SNAPSHOT_H
#include <stdint.h>
typedef struct {
    struct { struct { uint8_t mcu_detc, mcu_sw_present; } input; } signal;
    struct { uint8_t u8display_soc; } soc;
    uint8_t bms_work_status;
} app_bms_snapshot_t;
void app_bms_snapshot_read(app_bms_snapshot_t *snapshot);
#endif
