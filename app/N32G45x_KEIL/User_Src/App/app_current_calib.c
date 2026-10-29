/*
 * 文件作用：
 *   实现主回路电流的统一K/B校准、Flash持久化和校准后电流输出。
 *
 * 主要用途：
 *   上位机通过0x71发送有符号实际电流值：0mA可选地采集零点，正数或负数均可作为
 *   已知电流校准点。本模块对充电和放电共用同一组K/B参数，默认零点原始值为0。
 *
 * 适用范围：
 *   当前DVC1124 AFE主回路电流链路。更换AFE时，只要继续提供hsi_afe_t.curr_raw
 *   和hsi_afe_t.current_update_seq，本模块的公式、协议接口和Flash格式不需要改变。
 *
 * 调用关系：
 *   app_current_calib_init()在AFE对象初始化后调用；app_current_calib_task()由AFE_Task以10ms周期调用，
 *   通过current_update_seq避免重复累计同一个AFE旧样本。
 *   本模块读取gt_hsi_afe.curr_raw，写回gt_hsi_afe.curr_calibrated和gt_hsi_afe.curr_real，
 *   业务快照再读取这两个值。current_update_seq用于避免重复累计同一个AFE旧样本。
 *
 * 注意事项：
 *   1. 电流校准记录与SN、产品模式和电压校准共用Page0，更新时保留其他记录。
 *   2. K使用K_PPM保存，避免直接发送浮点数，也避免固定Q格式的整数溢出和歧义。
 *   3. 0A点为可选项；未单独校准时默认AFE原始零点R0=0，推荐标定时仍先采集0A。
 *   4. 当前AFE电流是分步采样，通常约700ms才产生一个新电流值；5个新样本的校准时间约3.5s，
 *      不是简单的10次100ms重复读取。
 */
#include "App/app_current_calib.h"

#include "App/app_flash_map.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "HSI_afe/hsi_afe.h"

#include <string.h>

#ifdef APP_CURRENT_CALIB_HOST_TEST
extern void app_current_calib_test_critical_enter(void);
extern void app_current_calib_test_critical_exit(void);
#define APP_CAL2_CRITICAL_ENTER() app_current_calib_test_critical_enter()
#define APP_CAL2_CRITICAL_EXIT()  app_current_calib_test_critical_exit()
#else
#include "FreeRTOS.h"
#include "task.h"
#define APP_CAL2_CRITICAL_ENTER() taskENTER_CRITICAL()
#define APP_CAL2_CRITICAL_EXIT()  taskEXIT_CRITICAL()
#endif

#pragma pack(push, 1)
typedef struct
{
    uint16_t tag;                 /* Flash中可直接识别为“UC”。 */
    uint32_t k_ppm;               /* 充放电共用K乘以1000000后的整数。 */
    int32_t b_ma;                 /* 充放电共用B，单位mA。 */
    int32_t zero_raw_ma;          /* 可选0A点的AFE原始电流平均值，默认0mA。 */
} app_current_calib_flash_t;
#pragma pack(pop)

typedef char app_current_calib_record_must_end_at_voltage_offset[
    ((APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET + sizeof(app_current_calib_flash_t)) ==
     APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET) ? 1 : -1];
typedef char app_current_calib_record_must_be_14_bytes[
    (sizeof(app_current_calib_flash_t) == 14U) ? 1 : -1];

#ifdef APP_CURRENT_CALIB_HOST_TEST
extern const uint8_t *app_current_calib_test_flash_ptr(uint32_t addr);
#define APP_CAL2_FLASH_PTR(addr) app_current_calib_test_flash_ptr(addr)
#else
#define APP_CAL2_FLASH_PTR(addr) ((const uint8_t *)(addr))
#endif

/* 在共享参数页更新14字节UC记录，整页其他数据由参数模块保留。 */
static bool app_current_calib_save_values(uint32_t k_ppm, int32_t b_ma, int32_t zero_raw_ma)
{
    app_current_calib_flash_t record;
    record.tag = APP_FLASH_TAG_CURRENT_CALIB;
    record.k_ppm = k_ppm;
    record.b_ma = b_ma;
    record.zero_raw_ma = zero_raw_ma;
    return app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET,
                                       (const uint8_t *)&record,
                                       (uint32_t)sizeof(record));
}

typedef struct
{
    uint32_t k_ppm;               /* 当前充放电共用K_PPM。 */
    int32_t b_ma;                 /* 当前充放电共用B，单位mA。 */
    int32_t zero_raw_ma;          /* 0A原始值；未校0A时默认为0。 */
    int32_t target_ma;            /* 当前等待采集的实际电流点，有符号，单位mA。 */
    int64_t sample_sum_ma;        /* 当前校准点的原始电流累加和。 */
    uint32_t sample_last_seq;     /* 上一次累计样本对应的AFE电流刷新序号。 */
    uint8_t busy;                 /* 是否正在采样。 */
    uint8_t sample_count;         /* 已完成的新样本数量。 */
} app_current_calib_state_t;

static app_current_calib_state_t s_app_current_calib;

typedef enum
{
    APP_CAL2_IDLE = 0U,
    APP_CAL2_WAIT_FIRST,
    APP_CAL2_SAMPLE_FIRST,
    APP_CAL2_WAIT_SECOND,
    APP_CAL2_SAMPLE_SECOND,
    APP_CAL2_SAVING,
    APP_CAL2_SAVED,
    APP_CAL2_FAILED,
    APP_CAL2_CANCELED,
    APP_CAL2_EXPIRED
} app_cal2_phase_t;

typedef struct
{
    app_cal2_phase_t phase;
    uint16_t session;
    uint8_t source;
    uint8_t max_sequence;
    uint8_t begin_sequence;
    uint8_t first_sequence;
    uint8_t second_sequence;
    uint8_t query_sequence;
    uint8_t cancel_sequence;
    int32_t first_reference;
    int32_t second_reference;
    int32_t raw_first;
    int32_t sample_min;
    int32_t sample_max;
    int64_t sample_sum;
    uint8_t sample_count;
    uint32_t sample_sequence;
    uint32_t begin_ms;
    uint32_t point_ms;
    uint32_t terminal_ms;
    uint8_t terminal_status;
    uint8_t completion_pending;
    app_current_calib_cal2_completion_t completion;
} app_cal2_state_t;

static app_cal2_state_t s_cal2;
static uint32_t s_cal2_now_ms;
static uint32_t s_cal2_latest_sample_ms;
static uint32_t s_cal2_latest_sample_seq;

static bool app_cal2_is_active_unlocked(void)
{
    return (s_cal2.phase >= APP_CAL2_WAIT_FIRST) &&
           (s_cal2.phase <= APP_CAL2_SAVING);
}

bool app_current_calib_cal2_is_active(void)
{
    bool active;
    APP_CAL2_CRITICAL_ENTER();
    active = app_cal2_is_active_unlocked();
    APP_CAL2_CRITICAL_EXIT();
    return active;
}

bool app_current_calib_cal2_has_completion(void)
{
    bool pending;
    APP_CAL2_CRITICAL_ENTER();
    pending = (s_cal2.completion_pending != 0U);
    APP_CAL2_CRITICAL_EXIT();
    return pending;
}

static void app_cal2_finish(uint8_t status)
{
    uint8_t operation = (s_cal2.phase == APP_CAL2_SAMPLE_FIRST) ? 2U : 3U;
    s_cal2.phase = APP_CAL2_FAILED;
    s_cal2.terminal_status = status;
    s_cal2.terminal_ms = s_cal2_now_ms;
    if ((s_cal2.first_sequence != 0U) || (s_cal2.second_sequence != 0U))
    {
        s_cal2.completion.status = status;
        s_cal2.completion.operation = operation;
        s_cal2.completion.sequence = (s_cal2.completion.operation == 2U) ?
                                      s_cal2.first_sequence : s_cal2.second_sequence;
        s_cal2.completion.session = s_cal2.session;
        s_cal2.completion.destination = s_cal2.source;
        s_cal2.completion_pending = 1U;
    }
}

void app_current_calib_cal2_advance_ms(uint32_t elapsed_ms)
{
    APP_CAL2_CRITICAL_ENTER();
    s_cal2_now_ms += elapsed_ms;
    if ((s_cal2.phase != APP_CAL2_SAVING) &&
        app_cal2_is_active_unlocked() &&
        ((uint32_t)(s_cal2_now_ms - s_cal2.begin_ms) >= 120000UL))
    {
        s_cal2.phase = APP_CAL2_EXPIRED;
        s_cal2.terminal_status = 8U;
        s_cal2.terminal_ms = s_cal2_now_ms;
        s_cal2.completion_pending = 0U;
    }
    else if (((s_cal2.phase == APP_CAL2_SAMPLE_FIRST) ||
              (s_cal2.phase == APP_CAL2_SAMPLE_SECOND)) &&
             ((uint32_t)(s_cal2_now_ms - s_cal2.point_ms) >= 15000UL))
    {
        app_cal2_finish(0x0CU);
    }
    if ((s_cal2.phase >= APP_CAL2_SAVED) &&
        ((uint32_t)(s_cal2_now_ms - s_cal2.terminal_ms) >= 120000UL))
    {
        memset(&s_cal2, 0, sizeof(s_cal2));
    }
    APP_CAL2_CRITICAL_EXIT();
}

static bool app_cal2_afe_ready(void)
{
    return (gt_hsi_afe.data_ready != 0U) &&
           (gt_hsi_afe.status_valid != 0U) &&
           (gt_hsi_afe.status.data[0] == 0U) &&
           (s_cal2_latest_sample_seq != 0U) &&
           ((uint32_t)(s_cal2_now_ms - s_cal2_latest_sample_ms) <= 3000UL) &&
           (gt_hsi_afe.curr_reg > -524000) &&
           (gt_hsi_afe.curr_reg < 524000) &&
           (gt_hsi_afe.curr_raw >= -800000) &&
           (gt_hsi_afe.curr_raw <= 800000);
}

static uint8_t app_cal2_overall_status(void)
{
    switch (s_cal2.phase)
    {
        case APP_CAL2_WAIT_SECOND: return 0x0DU;
        case APP_CAL2_SAVED: return 3U;
        case APP_CAL2_FAILED:
        case APP_CAL2_CANCELED: return s_cal2.terminal_status;
        case APP_CAL2_EXPIRED: return 8U;
        default: return 9U;
    }
}

static uint8_t app_current_calib_cal2_request_unlocked(uint8_t operation,
                                                       uint8_t sequence,
                                                       uint16_t session,
                                                       int32_t reference_ma,
                                                       uint8_t source)
{
    uint8_t previous_status;
    if ((session == 0U) || (sequence == 0U) || (operation < 1U) ||
        (operation > 5U) || (((operation == 1U) || (operation >= 4U)) &&
                              (reference_ma != 0)))
    {
        return 1U;
    }
    if ((reference_ma < -800000) || (reference_ma > 800000))
    {
        return 6U;
    }
    if (operation == 1U)
    {
        if ((s_cal2.phase != APP_CAL2_IDLE) &&
            (s_cal2.session == session) && (s_cal2.source == source))
        {
            return (s_cal2.begin_sequence == sequence) ? 3U : 1U;
        }
        if (app_cal2_is_active_unlocked() || (s_cal2.completion_pending != 0U) ||
            (s_app_current_calib.busy != 0U) ||
            !app_cal2_afe_ready())
        {
            return 2U;
        }
        memset(&s_cal2, 0, sizeof(s_cal2));
        s_cal2.phase = APP_CAL2_WAIT_FIRST;
        s_cal2.source = source;
        s_cal2.session = session;
        s_cal2.begin_sequence = sequence;
        s_cal2.max_sequence = sequence;
        s_cal2.begin_ms = s_cal2_now_ms;
        return 3U;
    }
    if ((s_cal2.phase == APP_CAL2_IDLE) || (s_cal2.session != session) ||
        (s_cal2.source != source) || (s_cal2.phase == APP_CAL2_EXPIRED))
    {
        return 8U;
    }
    if ((s_cal2.phase == APP_CAL2_SAVING) && (operation == 4U))
    {
        return 2U;
    }
    if ((operation == 2U) && (s_cal2.first_sequence == sequence) &&
        (s_cal2.first_reference == reference_ma))
    {
        return (s_cal2.phase == APP_CAL2_SAMPLE_FIRST) ? 0U :
               ((s_cal2.phase == APP_CAL2_WAIT_SECOND) ||
                (s_cal2.phase == APP_CAL2_SAMPLE_SECOND) ||
                (s_cal2.phase == APP_CAL2_SAVED)) ? 3U : app_cal2_overall_status();
    }
    if ((operation == 3U) && (s_cal2.second_sequence == sequence) &&
        (s_cal2.second_reference == reference_ma))
    {
        return (s_cal2.phase == APP_CAL2_SAMPLE_SECOND) ? 0U : app_cal2_overall_status();
    }
    if ((operation == 5U) && (s_cal2.query_sequence == sequence))
    {
        return app_cal2_overall_status();
    }
    if ((operation == 4U) && (s_cal2.cancel_sequence == sequence))
    {
        return app_cal2_overall_status();
    }
    if (sequence <= s_cal2.max_sequence)
    {
        return 1U;
    }
    s_cal2.max_sequence = sequence;
    if (operation == 5U)
    {
        s_cal2.query_sequence = sequence;
        return app_cal2_overall_status();
    }
    if (operation == 4U)
    {
        s_cal2.cancel_sequence = sequence;
        if (s_cal2.phase == APP_CAL2_SAVED)
        {
            return 3U;
        }
        if (app_cal2_is_active_unlocked())
        {
            s_cal2.phase = APP_CAL2_CANCELED;
            s_cal2.terminal_status = 0x0BU;
            s_cal2.terminal_ms = s_cal2_now_ms;
            s_cal2.completion_pending = 0U;
            return 0x0BU;
        }
        return app_cal2_overall_status();
    }
    if (((operation == 2U) && (s_cal2.phase != APP_CAL2_WAIT_FIRST)) ||
        ((operation == 3U) && (s_cal2.phase != APP_CAL2_WAIT_SECOND)))
    {
        return 1U;
    }
    if (!app_cal2_afe_ready())
    {
        return 2U;
    }
    s_cal2.sample_count = 0U;
    s_cal2.sample_sum = 0LL;
    s_cal2.sample_min = 2147483647L;
    s_cal2.sample_max = (-2147483647L - 1L);
    s_cal2.sample_sequence = gt_hsi_afe.current_update_seq;
    s_cal2.point_ms = s_cal2_now_ms;
    if (operation == 2U)
    {
        s_cal2.first_sequence = sequence;
        s_cal2.first_reference = reference_ma;
        s_cal2.phase = APP_CAL2_SAMPLE_FIRST;
    }
    else
    {
        s_cal2.second_sequence = sequence;
        s_cal2.second_reference = reference_ma;
        s_cal2.phase = APP_CAL2_SAMPLE_SECOND;
    }
    previous_status = 0U;
    return previous_status;
}

uint8_t app_current_calib_cal2_request(uint8_t operation, uint8_t sequence,
                                       uint16_t session, int32_t reference_ma,
                                       uint8_t source)
{
    uint8_t status;
    APP_CAL2_CRITICAL_ENTER();
    status = app_current_calib_cal2_request_unlocked(operation, sequence, session,
                                                      reference_ma, source);
    APP_CAL2_CRITICAL_EXIT();
    return status;
}

bool app_current_calib_cal2_take_completion(app_current_calib_cal2_completion_t *completion)
{
    bool available = false;
    APP_CAL2_CRITICAL_ENTER();
    if ((completion != 0) && (s_cal2.completion_pending != 0U))
    {
        *completion = s_cal2.completion;
        s_cal2.completion_pending = 0U;
        available = true;
    }
    APP_CAL2_CRITICAL_EXIT();
    return available;
}

/* 将64位计算结果限制到int32_t，避免异常参数导致业务电流溢出。 */
static int32_t app_current_calib_saturate_i32(int64_t value)
{
    if (value > 2147483647LL)
    {
        return 2147483647L;
    }

    if (value < (-2147483647LL - 1LL))
    {
        return (-2147483647L - 1L);
    }

    return (int32_t)value;
}

/* 返回有符号整数的绝对值，使用int64_t避免INT32_MIN取绝对值溢出。 */
static int64_t app_current_calib_abs_i64(int64_t value)
{
    return (value < 0LL) ? -value : value;
}

app_current_calib_cal2_fit_result_t app_current_calib_cal2_fit(
    int32_t raw1_ma, int32_t actual1_ma, int32_t raw2_ma, int32_t actual2_ma,
    uint32_t *k_ppm, int32_t *b_ma)
{
    int64_t dx = (int64_t)raw2_ma - raw1_ma;
    int64_t dy = (int64_t)actual2_ma - actual1_ma;
    int64_t slope_ppm;
    int64_t bias_ma;
    int64_t second_ma;

    if ((k_ppm == 0) || (b_ma == 0) ||
        (app_current_calib_abs_i64(raw1_ma) > 800000LL) ||
        (app_current_calib_abs_i64(raw2_ma) > 800000LL) ||
        (app_current_calib_abs_i64(actual1_ma) > 800000LL) ||
        (app_current_calib_abs_i64(actual2_ma) > 800000LL))
    {
        return APP_CURRENT_CALIB_CAL2_FIT;
    }
    if ((app_current_calib_abs_i64(dx) < 2000LL) ||
        (app_current_calib_abs_i64(dy) < 2000LL))
    {
        return APP_CURRENT_CALIB_CAL2_SPACING;
    }
    if (((dx < 0LL) != (dy < 0LL)) || (dx == 0LL))
    {
        return APP_CURRENT_CALIB_CAL2_FIT;
    }

    slope_ppm = ((app_current_calib_abs_i64(dy) * 1000000LL) +
                 (app_current_calib_abs_i64(dx) / 2LL)) /
                app_current_calib_abs_i64(dx);
    if ((slope_ppm < 500000LL) || (slope_ppm > 2000000LL))
    {
        return APP_CURRENT_CALIB_CAL2_FIT;
    }
    bias_ma = (int64_t)actual1_ma - (slope_ppm * raw1_ma) / 1000000LL;
    second_ma = ((slope_ppm * raw2_ma) / 1000000LL) + bias_ma;
    if ((app_current_calib_abs_i64(bias_ma) > 20000LL) ||
        (app_current_calib_abs_i64(second_ma - actual2_ma) > 2LL))
    {
        return APP_CURRENT_CALIB_CAL2_FIT;
    }
    *k_ppm = (uint32_t)slope_ppm;
    *b_ma = (int32_t)bias_ma;
    return APP_CURRENT_CALIB_CAL2_OK;
}

bool app_current_calib_calculate_zero_bias(uint32_t k_ppm,
                                           int32_t zero_raw_ma,
                                           int32_t *b_ma)
{
    int64_t s64BiasMa;

    if ((k_ppm == 0U) || (b_ma == 0))
    {
        return false;
    }

    s64BiasMa = -(((int64_t)k_ppm * (int64_t)zero_raw_ma) / 1000000LL);
    *b_ma = app_current_calib_saturate_i32(s64BiasMa);
    return true;
}

/* 根据一个有符号实际校准点计算共用K/B，K始终保存为正值。 */
static bool app_current_calib_calculate_point(int32_t zero_raw_ma,
                                              int32_t point_raw_ma,
                                              int32_t actual_point_ma,
                                              uint32_t *k_ppm,
                                              int32_t *b_ma)
{
    int64_t s64RawDelta;
    int64_t s64Numerator;
    int64_t s64KValuePpm;

    if ((actual_point_ma == 0) || (k_ppm == 0) || (b_ma == 0))
    {
        return false;
    }

    s64RawDelta = (int64_t)point_raw_ma - (int64_t)zero_raw_ma;
    if ((s64RawDelta == 0LL) ||
        (((int64_t)actual_point_ma > 0LL) && (s64RawDelta < 0LL)) ||
        (((int64_t)actual_point_ma < 0LL) && (s64RawDelta > 0LL)))
    {
        /* 报文方向和AFE实测方向不一致时拒绝保存，防止校准方向写反。 */
        return false;
    }

    s64Numerator = app_current_calib_abs_i64((int64_t)actual_point_ma) * 1000000LL;
    s64KValuePpm = s64Numerator / app_current_calib_abs_i64(s64RawDelta);
    if ((s64KValuePpm <= 0LL) || (s64KValuePpm > 4294967295LL))
    {
        return false;
    }

    *k_ppm = (uint32_t)s64KValuePpm;
    return app_current_calib_calculate_zero_bias(*k_ppm, zero_raw_ma, b_ma);
}

bool app_current_calib_calculate_high(int32_t zero_raw_ma,
                                      int32_t high_raw_ma,
                                      int32_t actual_high_ma,
                                      uint32_t *k_ppm,
                                      int32_t *b_ma)
{
    return app_current_calib_calculate_point(zero_raw_ma,
                                             high_raw_ma,
                                             actual_high_ma,
                                             k_ppm,
                                             b_ma);
}

int32_t app_current_calib_calculate_output(uint32_t k_ppm,
                                           int32_t b_ma,
                                           int32_t raw_ma)
{
    int64_t s64CalibratedOutputMa;

    if (k_ppm == 0U)
    {
        k_ppm = APP_CURRENT_CALIB_K_DEFAULT_PPM;
    }

    s64CalibratedOutputMa = (((int64_t)k_ppm * (int64_t)raw_ma) / 1000000LL) + (int64_t)b_ma;
    return app_current_calib_saturate_i32(s64CalibratedOutputMa);
}

/* 所有新校准（包含旧接口）统一保存到共享参数页。 */
static bool app_current_calib_save(void)
{
    return app_current_calib_save_values(s_app_current_calib.k_ppm,
                                 s_app_current_calib.b_ma,
                                 s_app_current_calib.zero_raw_ma);
}

/* 从共享参数页恢复UC记录；无有效记录时使用默认K=1、B=0、R0=0。 */
static void app_current_calib_load(void)
{
    const app_current_calib_flash_t *ptFlashRecord;
    ptFlashRecord = (const app_current_calib_flash_t *)APP_CAL2_FLASH_PTR(
        APP_FLASH_MAP_DB_PARAM_ADDR + APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET);
    if ((ptFlashRecord->tag != APP_FLASH_TAG_CURRENT_CALIB) || (ptFlashRecord->k_ppm == 0U))
    {
        return;
    }

    s_app_current_calib.k_ppm = ptFlashRecord->k_ppm;
    s_app_current_calib.b_ma = ptFlashRecord->b_ma;
    s_app_current_calib.zero_raw_ma = ptFlashRecord->zero_raw_ma;
}

/* 把共用校准参数写回AFE对象，再由业务快照读取。 */
static void app_current_calib_apply(void)
{
    int32_t s32CalibratedMa;
    int32_t s32DeadbandMa;

    s32CalibratedMa = app_current_calib_calculate_output(s_app_current_calib.k_ppm,
                                                         s_app_current_calib.b_ma,
                                                         gt_hsi_afe.curr_raw);
    gt_hsi_afe.curr_calibrated = s32CalibratedMa;
    gt_hsi_afe.curr_real = s32CalibratedMa;

    s32DeadbandMa = (int32_t)APP_PARAM_CURRENT_ZERO_DEADBAND_MA;
    if ((s32DeadbandMa > 0) &&
        (gt_hsi_afe.curr_real > -s32DeadbandMa) &&
        (gt_hsi_afe.curr_real < s32DeadbandMa))
    {
        gt_hsi_afe.curr_real = 0;
    }
}

void app_current_calib_init(void)
{
    memset(&s_app_current_calib, 0, sizeof(s_app_current_calib));
    memset(&s_cal2, 0, sizeof(s_cal2));
    s_cal2_now_ms = 0U;
    s_cal2_latest_sample_ms = 0U;
    s_cal2_latest_sample_seq = 0U;
    s_app_current_calib.k_ppm = APP_CURRENT_CALIB_K_DEFAULT_PPM;
    /* 0A校准可选；未校准时AFE原始零点默认按0mA处理。 */
    s_app_current_calib.zero_raw_ma = 0;
    s_app_current_calib.sample_last_seq = gt_hsi_afe.current_update_seq;
    app_current_calib_load();
    app_current_calib_apply();
}

bool app_current_calib_submit(int32_t actual_current_ma)
{
    bool accepted = false;
    APP_CAL2_CRITICAL_ENTER();
    if ((s_app_current_calib.busy == 0U) && !app_cal2_is_active_unlocked() &&
        (s_cal2.completion_pending == 0U))
    {
        s_app_current_calib.target_ma = actual_current_ma;
        if (s_cal2.phase >= APP_CAL2_SAVED)
        {
            memset(&s_cal2, 0, sizeof(s_cal2));
        }
        s_app_current_calib.sample_sum_ma = 0;
        s_app_current_calib.sample_count = 0U;
        s_app_current_calib.sample_last_seq = gt_hsi_afe.current_update_seq;
        s_app_current_calib.busy = 1U;
        accepted = true;
    }
    APP_CAL2_CRITICAL_EXIT();
    return accepted;
}

bool app_current_calib_get(uint32_t *k_ppm, int32_t *b_ma)
{
    if ((k_ppm == 0) || (b_ma == 0))
    {
        return false;
    }

    APP_CAL2_CRITICAL_ENTER();
    *k_ppm = s_app_current_calib.k_ppm;
    *b_ma = s_app_current_calib.b_ma;
    APP_CAL2_CRITICAL_EXIT();
    return true;
}

bool app_current_calib_is_busy(void)
{
    bool busy;
    APP_CAL2_CRITICAL_ENTER();
    busy = (s_app_current_calib.busy != 0U) || app_cal2_is_active_unlocked();
    APP_CAL2_CRITICAL_EXIT();
    return busy;
}

static void app_cal2_sample_task(void)
{
    int32_t raw;
    int32_t average;
    uint32_t k_ppm = 0U;
    int32_t b_ma = 0;
    int32_t zero_raw = 0;
    uint16_t save_session = 0U;
    bool save_candidate = false;
    bool save_ok;
    app_current_calib_cal2_fit_result_t fit_result;

    APP_CAL2_CRITICAL_ENTER();
    if (gt_hsi_afe.current_update_seq != s_cal2_latest_sample_seq)
    {
        s_cal2_latest_sample_seq = gt_hsi_afe.current_update_seq;
        s_cal2_latest_sample_ms = s_cal2_now_ms;
    }
    if ((s_cal2.phase != APP_CAL2_SAMPLE_FIRST) &&
        (s_cal2.phase != APP_CAL2_SAMPLE_SECOND))
    {
        goto sample_done;
    }
    if (gt_hsi_afe.current_update_seq == s_cal2.sample_sequence)
    {
        goto sample_done;
    }
    s_cal2.sample_sequence = gt_hsi_afe.current_update_seq;
    if (!app_cal2_afe_ready())
    {
        app_cal2_finish(0x0AU);
        goto sample_done;
    }
    raw = gt_hsi_afe.curr_raw;
    if (raw < s_cal2.sample_min) s_cal2.sample_min = raw;
    if (raw > s_cal2.sample_max) s_cal2.sample_max = raw;
    s_cal2.sample_sum += raw;
    ++s_cal2.sample_count;
    if (s_cal2.sample_count < APP_CURRENT_CALIB_SAMPLE_COUNT)
    {
        goto sample_done;
    }
    average = (int32_t)(s_cal2.sample_sum / (int64_t)APP_CURRENT_CALIB_SAMPLE_COUNT);
    if ((int64_t)s_cal2.sample_max - s_cal2.sample_min >
        ((app_current_calib_abs_i64(average) / 100LL > 100LL) ?
         app_current_calib_abs_i64(average) / 100LL : 100LL))
    {
        app_cal2_finish(4U);
        goto sample_done;
    }
    if (s_cal2.phase == APP_CAL2_SAMPLE_FIRST)
    {
        s_cal2.raw_first = average;
        s_cal2.phase = APP_CAL2_WAIT_SECOND;
        s_cal2.completion.status = 3U;
        s_cal2.completion.operation = 2U;
        s_cal2.completion.sequence = s_cal2.first_sequence;
        s_cal2.completion.session = s_cal2.session;
        s_cal2.completion.destination = s_cal2.source;
        s_cal2.completion_pending = 1U;
        goto sample_done;
    }

    fit_result = app_current_calib_cal2_fit(s_cal2.raw_first,
                                             s_cal2.first_reference,
                                             average,
                                             s_cal2.second_reference,
                                             &k_ppm, &b_ma);
    if (fit_result != APP_CURRENT_CALIB_CAL2_OK)
    {
        app_cal2_finish((fit_result == APP_CURRENT_CALIB_CAL2_SPACING) ? 5U : 6U);
        goto sample_done;
    }
    zero_raw = (s_cal2.first_reference == 0) ? s_cal2.raw_first :
               (s_cal2.second_reference == 0) ? average :
               (int32_t)(-((int64_t)b_ma * 1000000LL) / (int64_t)k_ppm);
    save_session = s_cal2.session;
    s_cal2.phase = APP_CAL2_SAVING;
    save_candidate = true;

sample_done:
    APP_CAL2_CRITICAL_EXIT();

    if (!save_candidate)
    {
        return;
    }

    save_ok = app_current_calib_save_values(k_ppm, b_ma, zero_raw);

    APP_CAL2_CRITICAL_ENTER();
    if ((s_cal2.phase == APP_CAL2_SAVING) && (s_cal2.session == save_session))
    {
        if (!save_ok)
        {
            app_cal2_finish(7U);
        }
        else
        {
            s_app_current_calib.k_ppm = k_ppm;
            s_app_current_calib.b_ma = b_ma;
            s_app_current_calib.zero_raw_ma = zero_raw;
            s_cal2.phase = APP_CAL2_SAVED;
            s_cal2.terminal_status = 3U;
            s_cal2.terminal_ms = s_cal2_now_ms;
            s_cal2.completion.status = 3U;
            s_cal2.completion.operation = 3U;
            s_cal2.completion.sequence = s_cal2.second_sequence;
            s_cal2.completion.session = s_cal2.session;
            s_cal2.completion.destination = s_cal2.source;
            s_cal2.completion_pending = 1U;
        }
    }
    APP_CAL2_CRITICAL_EXIT();
}

void app_current_calib_task(void)
{
    int32_t s32AverageRawMa;
    uint32_t u32NewKFactorPpm;
    int32_t s32NewBiasMa;
    uint32_t u32OldKFactorPpm;
    int32_t s32OldBiasMa;
    int32_t s32OldZeroRawMa;
    uint8_t bUpdateSuccessful;

    app_cal2_sample_task();

    if ((s_app_current_calib.busy != 0U) &&
        (gt_hsi_afe.current_update_seq != s_app_current_calib.sample_last_seq))
    {
        /* 只在AFE产生新电流转换结果时累计，避免10ms任务重复使用旧缓存。 */
        s_app_current_calib.sample_last_seq = gt_hsi_afe.current_update_seq;
        s_app_current_calib.sample_sum_ma += (int64_t)gt_hsi_afe.curr_raw;
        s_app_current_calib.sample_count++;

        if (s_app_current_calib.sample_count >= APP_CURRENT_CALIB_SAMPLE_COUNT)
        {
            s32AverageRawMa = (int32_t)(s_app_current_calib.sample_sum_ma /
                                       (int64_t)APP_CURRENT_CALIB_SAMPLE_COUNT);

            u32OldKFactorPpm = s_app_current_calib.k_ppm;
            s32OldBiasMa = s_app_current_calib.b_ma;
            s32OldZeroRawMa = s_app_current_calib.zero_raw_ma;
            bUpdateSuccessful = 0U;

            if (s_app_current_calib.target_ma == 0)
            {
                s_app_current_calib.zero_raw_ma = s32AverageRawMa;
                (void)app_current_calib_calculate_zero_bias(s_app_current_calib.k_ppm,
                                                            s32AverageRawMa,
                                                            &s_app_current_calib.b_ma);
                bUpdateSuccessful = 1U;
            }
            else if (app_current_calib_calculate_point(s_app_current_calib.zero_raw_ma,
                                                       s32AverageRawMa,
                                                       s_app_current_calib.target_ma,
                                                       &u32NewKFactorPpm,
                                                       &s32NewBiasMa))
            {
                s_app_current_calib.k_ppm = u32NewKFactorPpm;
                s_app_current_calib.b_ma = s32NewBiasMa;
                bUpdateSuccessful = 1U;
            }

            if (bUpdateSuccessful != 0U)
            {
                if (!app_current_calib_save())
                {
                    /* Flash保存失败时恢复RAM中的旧参数，避免使用未持久化的校准结果。 */
                    s_app_current_calib.k_ppm = u32OldKFactorPpm;
                    s_app_current_calib.b_ma = s32OldBiasMa;
                    s_app_current_calib.zero_raw_ma = s32OldZeroRawMa;
                }
            }

            /* 保持busy直至Flash提交/回滚完成，避免新CAL2会话与旧校准保存重叠。 */
            APP_CAL2_CRITICAL_ENTER();
            s_app_current_calib.busy = 0U;
            APP_CAL2_CRITICAL_EXIT();
        }
    }

    /* 每次AFE_Task周期都重新应用一次，确保通信、保护、LOG看到同一条校准链路。 */
    app_current_calib_apply();
}
