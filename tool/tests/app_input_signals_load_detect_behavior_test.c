#include <stdint.h>
#include <stdio.h>

#include "App/app_input_signals.h"
#include "App/app_input_signals.c"

static uint8_t s_load_raw_level = 1U;
static uint8_t s_detd_raw_level = 1U;
static uint8_t s_detc_raw_level = 1U;
static uint8_t s_det2_raw_level = 1U;
static uint8_t s_sw_raw_level = 1U;

uint8_t board_io_load_detect_level(void) { return s_load_raw_level; }
uint8_t board_io_mcu_detd_level(void) { return s_detd_raw_level; }
uint8_t board_io_mcu_detc_level(void) { return s_detc_raw_level; }
uint8_t board_io_mcu_2nd_det_level(void) { return s_det2_raw_level; }
uint8_t board_io_mcu_sw_level(void) { return s_sw_raw_level; }

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s (line %d)\n", (message), __LINE__); \
            return 1; \
        } \
    } while (0)

int main(void)
{
    uint16_t i;

    app_input_signals_init();

    /* With LD disabled, the pulled-up pin ages into the cached removed state. */
    s_load_raw_level = 1U;
    for (i = 0U; i < 50U; ++i)
    {
        app_input_signals_update();
    }
    CHECK(gt_app_input_signals.mcu_det_load == 0U,
          "disabled detector should reproduce a cached removed state");

    /* Enabling LD must immediately discard that stale result and assume present. */
    s_load_raw_level = 0U;
    app_input_signals_load_detect_rearm();
    CHECK(gt_app_input_signals.mcu_det_load == 1U,
          "rearm must publish the conservative present state immediately");
    for (i = 0U; i < 60U; ++i)
    {
        app_input_signals_update();
    }
    CHECK(gt_app_input_signals.mcu_det_load == 1U,
          "a connected load must remain present after rearm");

    /* A real removal is accepted only after a fresh 500 ms of stable samples. */
    s_load_raw_level = 1U;
    app_input_signals_load_detect_rearm();
    CHECK(gt_app_input_signals.mcu_det_load == 1U,
          "rearm must mask the previous state while a new removal is qualified");
    for (i = 0U; i < 49U; ++i)
    {
        app_input_signals_update();
        CHECK(gt_app_input_signals.mcu_det_load == 1U,
              "removed state must not publish before the full debounce interval");
    }
    app_input_signals_update();
    CHECK(gt_app_input_signals.mcu_det_load == 0U,
          "stable removal must publish after 500 ms of fresh samples");

    puts("Load-detect rearm/debounce behavior: PASS");
    return 0;
}
