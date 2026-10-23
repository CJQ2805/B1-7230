#include "App/app_bms_status.h"
#include "App/app_bms_snapshot.h"
#include "cmsis_os.h"

#define APP_BMS_STATUS_STABLE_TIME_MS         (5000U)
#define APP_BMS_STATUS_MAX_SAMPLE_GAP_MS       (100U)
#define APP_BMS_STATUS_WINDOW_CAPACITY         (501U)
#define APP_BMS_STATUS_CURRENT_SPAN_10MA      (50)
#define APP_BMS_STATUS_MS_TO_TICKS(ms) \
    ((uint32_t)((((uint64_t)(ms) * (uint64_t)osKernelSysTickFrequency) + 999U) / 1000U))
#define APP_BMS_STATUS_STABLE_TIME_TICKS \
    APP_BMS_STATUS_MS_TO_TICKS(APP_BMS_STATUS_STABLE_TIME_MS)
#define APP_BMS_STATUS_MAX_SAMPLE_GAP_TICKS \
    APP_BMS_STATUS_MS_TO_TICKS(APP_BMS_STATUS_MAX_SAMPLE_GAP_MS)

static app_bms_snapshot_t s_status_snapshot;
static int16_t s_current_window_10ma[APP_BMS_STATUS_WINDOW_CAPACITY];
static uint32_t s_current_window_ticks[APP_BMS_STATUS_WINDOW_CAPACITY];
static uint32_t s_candidate_start_tick;
static uint32_t s_last_sample_tick;
static uint16_t s_current_window_head;
static uint16_t s_current_window_count;
static uint8_t s_candidate_active;
static uint8_t s_last_sample_valid;

static void app_bms_status_reset_current_window(void)
{
    s_current_window_head = 0U;
    s_current_window_count = 0U;
    s_candidate_active = 0U;
    s_last_sample_valid = 0U;
}

static void app_bms_status_start_current_window(uint32_t now_tick, int16_t current_10ma)
{
    app_bms_status_reset_current_window();
    s_candidate_start_tick = now_tick;
    s_last_sample_tick = now_tick;
    s_candidate_active = 1U;
    s_last_sample_valid = 1U;
    s_current_window_10ma[0] = current_10ma;
    s_current_window_ticks[0] = now_tick;
    s_current_window_count = 1U;
}

static void app_bms_status_expire_old_samples(uint32_t now_tick)
{
    while (s_current_window_count > 0U)
    {
        if ((uint32_t)(now_tick - s_current_window_ticks[s_current_window_head]) <=
            APP_BMS_STATUS_STABLE_TIME_TICKS)
        {
            break;
        }

        s_current_window_head++;
        if (s_current_window_head >= APP_BMS_STATUS_WINDOW_CAPACITY)
        {
            s_current_window_head = 0U;
        }
        s_current_window_count--;
    }
}

static uint8_t app_bms_status_update_current_window(uint32_t now_tick,
                                                    int16_t current_10ma)
{
    uint16_t index;
    uint16_t offset;
    int16_t min_current_10ma;
    int16_t max_current_10ma;
    int32_t current_span_10ma;

    if ((s_candidate_active == 0U) ||
        (s_last_sample_valid == 0U) ||
        ((uint32_t)(now_tick - s_last_sample_tick) > APP_BMS_STATUS_MAX_SAMPLE_GAP_TICKS))
    {
        /* 采样中断超过100ms时，无法证明期间电流连续稳定，从当前样本重新计时。 */
        app_bms_status_start_current_window(now_tick, current_10ma);
        return 0U;
    }

    s_last_sample_tick = now_tick;
    app_bms_status_expire_old_samples(now_tick);

    if (s_current_window_count >= APP_BMS_STATUS_WINDOW_CAPACITY)
    {
        s_current_window_head++;
        if (s_current_window_head >= APP_BMS_STATUS_WINDOW_CAPACITY)
        {
            s_current_window_head = 0U;
        }
        s_current_window_count--;
    }

    index = s_current_window_head + s_current_window_count;
    if (index >= APP_BMS_STATUS_WINDOW_CAPACITY)
    {
        index = (uint16_t)(index - APP_BMS_STATUS_WINDOW_CAPACITY);
    }
    s_current_window_10ma[index] = current_10ma;
    s_current_window_ticks[index] = now_tick;
    s_current_window_count++;

    index = s_current_window_head;
    min_current_10ma = s_current_window_10ma[index];
    max_current_10ma = s_current_window_10ma[index];
    for (offset = 1U; offset < s_current_window_count; offset++)
    {
        index++;
        if (index >= APP_BMS_STATUS_WINDOW_CAPACITY)
        {
            index = 0U;
        }
        if (s_current_window_10ma[index] < min_current_10ma)
        {
            min_current_10ma = s_current_window_10ma[index];
        }
        if (s_current_window_10ma[index] > max_current_10ma)
        {
            max_current_10ma = s_current_window_10ma[index];
        }
    }

    current_span_10ma = (int32_t)max_current_10ma - (int32_t)min_current_10ma;
    if (current_span_10ma > APP_BMS_STATUS_CURRENT_SPAN_10MA)
    {
        /* 超出500mA波动范围：回到回馈状态，并从当前样本重新确认5秒。 */
        app_bms_status_start_current_window(now_tick, current_10ma);
        return 0U;
    }

    return ((uint32_t)(now_tick - s_candidate_start_tick) >= APP_BMS_STATUS_STABLE_TIME_TICKS) ?
           1U : 0U;
}

void app_bms_status_task(void)
{
    int16_t current_10ma;
    uint8_t detc_present;
    uint8_t detd_present;
    uint8_t work_status;
    uint32_t now_tick;

    now_tick = osKernelSysTick();
    app_bms_snapshot_read(&s_status_snapshot);
    current_10ma = s_status_snapshot.current_10ma;
    detc_present = s_status_snapshot.signal.input.mcu_detc;
    detd_present = s_status_snapshot.signal.input.mcu_detd;

    if ((detc_present != 0U) && (current_10ma > 0))
    {
        /* DETC在位且检测到正向电流时，立即判定为充电，不等待稳定计时。 */
        app_bms_status_reset_current_window();
        work_status = APP_BMS_STATUS_CHARGE;
    }
    else if (current_10ma > 0)
    {
        /* DETC不在位时，正向电流先作为回馈；稳定5秒后再判为充电。 */
        work_status = (app_bms_status_update_current_window(now_tick, current_10ma) != 0U) ?
                      APP_BMS_STATUS_CHARGE : APP_BMS_STATUS_FEEDBACK;
    }
    else
    {
        /* 无正向电流时立即清除充电计时；负电流或DETD在位表示放电。 */
        app_bms_status_reset_current_window();
        work_status = ((current_10ma < 0) || (detd_present != 0U)) ?
                      APP_BMS_STATUS_DISCHARGE : APP_BMS_STATUS_STATIC;
    }

    app_bms_snapshot_set_work_status(work_status);
}
