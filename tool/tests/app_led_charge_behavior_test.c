#include <assert.h>
#include <string.h>
#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "App/app_led.h"

static app_bms_snapshot_t snapshot;
static uint8_t lights[5];
void app_bms_snapshot_read(app_bms_snapshot_t *out) { *out = snapshot; }
void board_io_mcu_ld1_set(uint8_t level) { lights[0] = level; }
void board_io_mcu_ld2_set(uint8_t level) { lights[1] = level; }
void board_io_mcu_ld3_set(uint8_t level) { lights[2] = level; }
void board_io_mcu_ld4_set(uint8_t level) { lights[3] = level; }
void board_io_mcu_ld5_set(uint8_t level) { lights[4] = level; }
static void expect_lights(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e)
{ const uint8_t expected[] = {a,b,c,d,e}; assert(memcmp(lights, expected, sizeof(expected)) == 0); }
static void run_cycles(unsigned count) { while (count-- != 0U) app_led_task(); }

int main(void)
{
    app_led_init();
    snapshot.signal.input.mcu_detc = 1U;
    snapshot.soc.u8display_soc = 45U;
    snapshot.bms_work_status = APP_BMS_STATUS_STATIC;
    run_cycles(120U);
    expect_lights(1,1,0,0,0); /* DETC plugged, 45%: three LEDs continuously on */
    snapshot.soc.u8display_soc = 0U;
    run_cycles(100U);
    expect_lights(1,1,1,1,0); /* plugged and empty: lowest LED continuously on */

    snapshot.signal.input.mcu_detc = 0U;
    snapshot.soc.u8display_soc = 45U;
    snapshot.bms_work_status = APP_BMS_STATUS_CHARGE;
    app_led_task();
    expect_lights(1,1,0,0,0);
    run_cycles(49U);
    expect_lights(1,1,1,0,0); /* charge animation follows status without DETC */
    snapshot.soc.u8display_soc = 100U;
    run_cycles(100U);
    expect_lights(0,0,0,0,0);
    snapshot.bms_work_status = APP_BMS_STATUS_STATIC;
    snapshot.signal.input.mcu_sw_present = 0U;
    app_led_task();
    expect_lights(1,1,1,1,1);
    snapshot.signal.input.mcu_sw_present = 1U;
    snapshot.soc.u8display_soc = 5U;
    app_led_task();
    expect_lights(1,1,1,1,0);
    run_cycles(24U);
    expect_lights(1,1,1,1,1); /* original low-SOC warning without DETC */
    return 0;
}
