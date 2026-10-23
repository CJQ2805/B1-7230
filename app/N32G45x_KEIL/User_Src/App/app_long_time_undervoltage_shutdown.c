#include "App/app_long_time_undervoltage_shutdown.h"

#include <stdint.h>

#include "App/app_bms_fault.h"
#include "App/app_bms_snapshot.h"
#include "App/app_power_manage.h"
#include "Driver/d_rtc.h"

#define APP_LONG_UV_SHUTDOWN_SECONDS ((uint64_t)86400UL)
#define APP_LONG_UV_RTC_CYCLE_SECONDS ((uint64_t)3155760000ULL) /* RTC两位年份2000~2099的完整周期。 */

static uint64_t s_app_long_uv_start_seconds;
static uint8_t s_app_long_uv_tracking;
static uint8_t s_app_long_uv_start_was_calibrated;
static uint8_t s_app_long_uv_shutdown_requested;

static uint8_t app_long_uv_is_leap_year(uint16_t year)
{
    if ((year % 400U) == 0U)
    {
        return 1U;
    }

    if ((year % 100U) == 0U)
    {
        return 0U;
    }

    return ((year % 4U) == 0U) ? 1U : 0U;
}

static uint8_t app_long_uv_month_days(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] =
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    if ((month == 2U) && (app_long_uv_is_leap_year(year) != 0U))
    {
        return 29U;
    }

    return days[month - 1U];
}

static uint64_t app_long_uv_calendar_to_seconds(const d_rtc_time_t *time)
{
    uint16_t year;
    uint8_t month;
    uint64_t days;

    days = 0ULL;
    for (year = 2000U; year < time->year; year++)
    {
        days += (app_long_uv_is_leap_year(year) != 0U) ? 366ULL : 365ULL;
    }

    for (month = 1U; month < time->month; month++)
    {
        days += app_long_uv_month_days(time->year, month);
    }
    days += (uint64_t)(time->day - 1U);

    return (((days * 24ULL + time->hour) * 60ULL + time->min) * 60ULL + time->sec);
}

void app_long_time_undervoltage_shutdown_init(void)
{
    s_app_long_uv_start_seconds = 0ULL;
    s_app_long_uv_tracking = 0U;
    s_app_long_uv_start_was_calibrated = 0U;
    s_app_long_uv_shutdown_requested = 0U;
}

void app_long_time_undervoltage_shutdown_task(void)
{
    struct _app_bms_fault_t fault_snapshot;
    app_bms_snapshot_t bms_snapshot;
    d_rtc_time_t rtc_time;
    uint8_t cuv1_active;
    uint8_t rtc_is_calibrated;
    uint64_t now_seconds;
    uint64_t elapsed_seconds;

    if (s_app_long_uv_shutdown_requested != 0U)
    {
        return;
    }

    app_bms_snapshot_read(&bms_snapshot);
    app_bms_fault_snapshot_read(&fault_snapshot);

    if ((bms_snapshot.data_ready == 0U) || (fault_snapshot.u8data_init_flag == 0U))
    {
        s_app_long_uv_tracking = 0U;
        return;
    }

    /* CUV2单独在BMS任务中立即请求关机；深度欠压由PF立即关机。这里只累计CUV1。 */
    cuv1_active =
        (fault_snapshot.tbms_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 != 0U) ? 1U : 0U;

    if ((cuv1_active == 0U) || (bms_snapshot.current_ma > 0))
    {
        s_app_long_uv_tracking = 0U;
        return;
    }

    if (!d_rtc_read_calendar(&rtc_time))
    {
        return;
    }

    now_seconds = app_long_uv_calendar_to_seconds(&rtc_time);
    rtc_is_calibrated = d_rtc_is_valid() ? 1U : 0U;
    if (s_app_long_uv_tracking == 0U)
    {
        s_app_long_uv_start_seconds = now_seconds;
        s_app_long_uv_start_was_calibrated = rtc_is_calibrated;
        s_app_long_uv_tracking = 1U;
        return;
    }

    elapsed_seconds = 0ULL;
    if (now_seconds >= s_app_long_uv_start_seconds)
    {
        elapsed_seconds = now_seconds - s_app_long_uv_start_seconds;
    }
    else if ((s_app_long_uv_start_was_calibrated == 0U) && (rtc_is_calibrated != 0U))
    {
        /* 首次校时可能让两位年份从如2070映射到2026；按100年周期识别为向前校时。 */
        elapsed_seconds = (APP_LONG_UV_RTC_CYCLE_SECONDS - s_app_long_uv_start_seconds) + now_seconds;
    }

    if (elapsed_seconds >= APP_LONG_UV_SHUTDOWN_SECONDS)
    {
        if (app_power_manage_request_shutdown() == APP_RESULT_OK)
        {
            s_app_long_uv_shutdown_requested = 1U;
        }
    }
}
