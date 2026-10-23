#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "App/app_bms_status.h"
#include "App/app_bms_snapshot.h"

static app_bms_snapshot_t s_snapshot;
static uint32_t s_test_tick;

uint32_t osKernelSysTick(void)
{
    return s_test_tick;
}

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot)
{
    *snapshot = s_snapshot;
}

void app_bms_snapshot_set_work_status(uint8_t bms_work_status)
{
    s_snapshot.bms_work_status = bms_work_status;
}

static void sample_status_at(uint32_t tick, int16_t current_10ma,
                             uint8_t detc, uint8_t detd)
{
    s_test_tick = tick;
    s_snapshot.current_10ma = current_10ma;
    s_snapshot.signal.input.mcu_detc = detc;
    s_snapshot.signal.input.mcu_detd = detd;
    app_bms_status_task();
}

static void sample_status(int16_t current_10ma, uint8_t detc, uint8_t detd)
{
    sample_status_at(s_test_tick, current_10ma, detc, detd);
    s_test_tick += 10U;
}

static void repeat_status(int16_t current_10ma, uint8_t detc,
                          uint8_t detd, uint16_t samples)
{
    uint16_t index;

    for (index = 0U; index < samples; index++)
    {
        sample_status(current_10ma, detc, detd);
    }
}

#define CHECK(condition, message) \
    do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", (message)); return 1; } } while (0)

int main(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));

    sample_status(0, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_STATIC,
          "no current and no DETD must be static");

    sample_status(0, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_DISCHARGE,
          "DETD present without current must be discharge");

    sample_status(-1, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_DISCHARGE,
          "negative processed current must be discharge even without DETD");

    sample_status(-1, 1U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_DISCHARGE,
          "DETC without positive current must not override discharge current");

    sample_status(100, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "positive current without DETC must be feedback even when DETD is present");

    repeat_status(100, 0U, 1U, 499U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "positive current must not be stable before a full 5-second window");
    sample_status(100, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
          "stable positive current must become charge after 5 seconds");

    sample_status(150, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
          "exactly 500mA peak-to-peak must remain stable");

    sample_status(151, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "more than 500mA peak-to-peak must immediately fall back to feedback");
    repeat_status(151, 0U, 1U, 499U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "a current excursion must require a fresh 5-second qualification");
    sample_status(151, 0U, 1U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
          "stable current after an excursion must requalify after 5 seconds");

    sample_status(100, 0U, 0U);
    repeat_status(100, 0U, 0U, 249U);
    sample_status(0, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_STATIC,
          "zero current must cancel qualification and select static without DETD");
    repeat_status(100, 0U, 0U, 500U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "positive-current qualification must restart after an interruption");
    sample_status(100, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
          "restarted positive-current qualification must complete after 5 seconds");

    sample_status(100, 0U, 0U);
    sample_status(-1, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_DISCHARGE,
          "negative current during qualification must cancel it and select discharge");

    sample_status(0, 1U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_STATIC,
          "DETC without positive current must not select charge");
    sample_status(100, 1U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
          "DETC with positive current must select charge immediately");
    sample_status(0, 1U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_STATIC,
          "DETC charge status must clear when positive current stops");
    sample_status(100, 0U, 0U);
    CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
          "removing DETC must require a fresh positive-current qualification");

    {
        uint32_t start_tick = 0xFFFFFF00UL;
        uint16_t index;

        sample_status_at(start_tick, 100, 0U, 0U);
        for (index = 1U; index <= 332U; index++)
        {
            sample_status_at(start_tick + ((uint32_t)index * 15U), 100, 0U, 0U);
        }
        CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
              "stable current observed for less than 5 seconds must remain feedback");

        sample_status_at(start_tick + 4999U, 100, 0U, 0U);
        CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
              "elapsed time of 4999ms must not qualify as stable charge");
        sample_status_at(start_tick + 5000U, 100, 0U, 0U);
        CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_CHARGE,
              "stable current must qualify at 5 seconds even with slower sampling");

        sample_status_at(start_tick + 5101U, 100, 0U, 0U);
        CHECK(s_snapshot.bms_work_status == APP_BMS_STATUS_FEEDBACK,
              "a sampling gap above 100ms must restart continuous-stability qualification");
    }

    puts("BMS status behavior tests passed");
    return 0;
}
