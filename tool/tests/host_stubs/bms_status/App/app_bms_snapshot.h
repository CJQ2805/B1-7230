#ifndef __APP_BMS_SNAPSHOT_H__
#define __APP_BMS_SNAPSHOT_H__

#include <stdint.h>

typedef struct
{
    struct
    {
        struct
        {
            uint8_t mcu_detc;
            uint8_t mcu_detd;
        } input;
    } signal;
    int16_t current_10ma;
    uint8_t bms_work_status;
} app_bms_snapshot_t;

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot);
void app_bms_snapshot_set_work_status(uint8_t bms_work_status);

#endif
