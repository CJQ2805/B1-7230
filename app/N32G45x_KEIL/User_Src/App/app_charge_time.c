#include "App/app_charge_time.h"

#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "Driver/d_timer.h"

#define APP_CHARGE_TIME_CONFIRM_MS          1000UL
#define APP_CHARGE_TIME_SAMPLE_INTERVAL_MS   300UL
#define APP_CHARGE_TIME_SAMPLE_COUNT         100U
/* AFE正常扫描远快于2秒；超过此时间没有新电流样本则估算失效。 */
#define APP_CHARGE_TIME_CURRENT_TIMEOUT_MS  2000UL
/* 相邻两次采样跳变超过3A时，用新电流重新累计估算窗口。 */
#define APP_CHARGE_TIME_CURRENT_STEP_MA     2000L
#define APP_CHARGE_TIME_JUMP_MINUTES          30U
#define APP_CHARGE_TIME_MIN_MINUTES           10U

/* 该模块只在BMS任务中更新，通信任务可通过getter读取16位结果。 */
static volatile uint16_t s_remaining_minutes = APP_CHARGE_TIME_INVALID_MINUTES;
static app_bms_snapshot_t s_charge_snapshot;
static uint32_t s_current_samples_ma[APP_CHARGE_TIME_SAMPLE_COUNT];
static uint32_t s_charge_start_ms;
static uint32_t s_last_sample_ms;
static uint32_t s_last_current_update_ms;
static uint32_t s_last_current_update_seq;
static uint32_t s_last_sampled_current_seq;
static int32_t s_last_sampled_current_ma;
static uint8_t s_sample_count;
static uint8_t s_charge_seen;
static uint8_t s_sampling;
static uint8_t s_current_seen;

static void app_charge_time_reset(void)
{
    s_charge_seen = 0U;
    s_sampling = 0U;
    s_sample_count = 0U;
    s_remaining_minutes = APP_CHARGE_TIME_INVALID_MINUTES;
}

static void app_charge_time_update_estimate(int32_t capacity_to_full_mah)
{
    uint64_t weighted_current_sum = 0ULL;
    uint64_t remaining_minutes;
    uint32_t average_current_ma;
    uint16_t candidate;
    uint16_t previous;
    uint16_t difference;
    uint8_t index;

    for (index = 0U; index < APP_CHARGE_TIME_SAMPLE_COUNT; ++index)
    {
        weighted_current_sum += (uint64_t)s_current_samples_ma[index] * (uint64_t)(index + 1U);
    }

    /* 1+2+...+100=5050；电流和容量均以mA/mAh计，结果乘60得到分钟。 */
    average_current_ma = (uint32_t)((weighted_current_sum + 2525ULL) / 5050ULL);
    if (average_current_ma == 0U)
    {
        return;
    }

    remaining_minutes = ((uint64_t)(uint32_t)capacity_to_full_mah * 60ULL) / average_current_ma;
    if (remaining_minutes >= (uint64_t)APP_CHARGE_TIME_INVALID_MINUTES)
    {
        candidate = (uint16_t)(APP_CHARGE_TIME_INVALID_MINUTES - 1U);
    }
    else if (remaining_minutes <= APP_CHARGE_TIME_MIN_MINUTES)
    {
        candidate = APP_CHARGE_TIME_MIN_MINUTES;
    }
    else
    {
        candidate = (uint16_t)remaining_minutes;
    }

    previous = s_remaining_minutes;
    difference = (candidate > previous) ? (uint16_t)(candidate - previous) :
                                        (uint16_t)(previous - candidate);
    /* 恒压阶段电流变小会拉长预计时间，及时上调，避免旧值长期偏低。 */
    if ((previous == APP_CHARGE_TIME_INVALID_MINUTES) ||
        (candidate > previous) ||
        (difference > APP_CHARGE_TIME_JUMP_MINUTES))
    {
        s_remaining_minutes = candidate;
    }
    else if ((candidate < previous) && (candidate > APP_CHARGE_TIME_MIN_MINUTES))
    {
        s_remaining_minutes = (uint16_t)(previous - 1U);
    }
    else if (candidate <= APP_CHARGE_TIME_MIN_MINUTES)
    {
        s_remaining_minutes = APP_CHARGE_TIME_MIN_MINUTES;
    }
}

void app_charge_time_task(void)
{
    uint32_t now_ms;
    int32_t capacity_to_full_mah;
    int64_t current_step_ma;

    app_bms_snapshot_read(&s_charge_snapshot);
    now_ms = d_timer_get_ms();

    if ((s_charge_snapshot.afe_current_update_seq != 0UL) &&
        ((s_current_seen == 0U) ||
         (s_charge_snapshot.afe_current_update_seq != s_last_current_update_seq)))
    {
        s_last_current_update_seq = s_charge_snapshot.afe_current_update_seq;
        s_last_current_update_ms = now_ms;
        s_current_seen = 1U;
    }

    if ((s_charge_snapshot.data_ready == 0U) ||
        (s_charge_snapshot.soc.s32battery_full_cap <= 0) ||
        (s_charge_snapshot.soc.s32remain_cap < 0) ||
        (s_charge_snapshot.soc.s32remain_cap > s_charge_snapshot.soc.s32battery_full_cap) ||
        (s_current_seen == 0U) ||
        ((uint32_t)(now_ms - s_last_current_update_ms) >= APP_CHARGE_TIME_CURRENT_TIMEOUT_MS))
    {
        app_charge_time_reset();
        return;
    }

    if ((s_charge_snapshot.soc.u8display_soc >= 100U) ||
        (s_charge_snapshot.soc.s32remain_cap == s_charge_snapshot.soc.s32battery_full_cap))
    {
        app_charge_time_reset();
        s_remaining_minutes = 0U;
        return;
    }

    if ((s_charge_snapshot.bms_work_status != APP_BMS_STATUS_CHARGE) ||
        (s_charge_snapshot.current_ma <= 0))
    {
        app_charge_time_reset();
        return;
    }

    capacity_to_full_mah = s_charge_snapshot.soc.s32battery_full_cap -
                           s_charge_snapshot.soc.s32remain_cap;

    if (s_charge_seen == 0U)
    {
        s_charge_seen = 1U;
        s_charge_start_ms = now_ms;
        return;
    }

    if ((uint32_t)(now_ms - s_charge_start_ms) < APP_CHARGE_TIME_CONFIRM_MS)
    {
        return;
    }

    if (s_sampling == 0U)
    {
        s_sampling = 1U;
        s_last_sample_ms = now_ms;
        s_last_sampled_current_seq = s_charge_snapshot.afe_current_update_seq;
        s_last_sampled_current_ma = s_charge_snapshot.current_ma;
        return;
    }

    if ((uint32_t)(now_ms - s_last_sample_ms) < APP_CHARGE_TIME_SAMPLE_INTERVAL_MS)
    {
        return;
    }
    if (s_charge_snapshot.afe_current_update_seq == s_last_sampled_current_seq)
    {
        return;
    }

    s_last_sample_ms = now_ms;
    s_last_sampled_current_seq = s_charge_snapshot.afe_current_update_seq;
    current_step_ma = (int64_t)s_charge_snapshot.current_ma -
                      (int64_t)s_last_sampled_current_ma;
    if ((current_step_ma >= APP_CHARGE_TIME_CURRENT_STEP_MA) ||
        (current_step_ma <= -APP_CHARGE_TIME_CURRENT_STEP_MA))
    {
        s_sample_count = 0U;
        s_remaining_minutes = APP_CHARGE_TIME_INVALID_MINUTES;
    }
    s_last_sampled_current_ma = s_charge_snapshot.current_ma;
    s_current_samples_ma[s_sample_count++] = (uint32_t)s_charge_snapshot.current_ma;
    if (s_sample_count >= APP_CHARGE_TIME_SAMPLE_COUNT)
    {
        app_charge_time_update_estimate(capacity_to_full_mah);
        s_sample_count = 0U;
    }
}

uint16_t app_charge_time_get_minutes(void)
{
    return s_remaining_minutes;
}
