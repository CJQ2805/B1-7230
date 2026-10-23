/*
 * 文件作用：采集板级输入，在模块内部完成电平解释、消抖和按键延时，
 *           对外只发布 app_input_signals_t 中的最终业务状态。
 * DataHandle_Task 每10ms调用一次本模块更新函数。
 */
#include "App/app_input_signals.h"
#include "Board/board_io.h"
#include "cmsis_os.h"
#include "task.h"
#include <string.h>

#define APP_INPUT_SIGNAL_SAMPLE_PERIOD_MS             10U
#define APP_INPUT_SIGNAL_DEBOUNCE_TIME_MS             500U
#define APP_INPUT_SIGNAL_DEBOUNCE_COUNT               \
    (APP_INPUT_SIGNAL_DEBOUNCE_TIME_MS / APP_INPUT_SIGNAL_SAMPLE_PERIOD_MS)

/* MCU_SW低电平按下；业务状态在内部消抖后再对外发布。 */
#define APP_INPUT_SIGNAL_MCU_SW_PRESSED_LEVEL         0U
#define APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_TIME_MS      20U
#define APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_COUNT        \
    (APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_TIME_MS / APP_INPUT_SIGNAL_SAMPLE_PERIOD_MS)
#define APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_TIME_MS  5000U
#define APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_COUNT    \
    (APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_TIME_MS / APP_INPUT_SIGNAL_SAMPLE_PERIOD_MS)
#define APP_INPUT_SIGNAL_CAN_KEY_TIMEOUT_MS            3000UL
#define APP_INPUT_SIGNAL_CAN_KEY_TIMEOUT_TICKS         \
    ((uint32_t)((((uint64_t)APP_INPUT_SIGNAL_CAN_KEY_TIMEOUT_MS * \
                   (uint64_t)osKernelSysTickFrequency) + 999U) / 1000U))

/* 物理检测输入为高电平表示未接入/已移除。 */
#define APP_INPUT_SIGNAL_MCU_DET_LOAD_REMOVED_LEVEL   1U
#define APP_INPUT_SIGNAL_MCU_DETD_REMOVED_LEVEL       1U
#define APP_INPUT_SIGNAL_MCU_DETC_REMOVED_LEVEL       1U

/* 以下均为消抖实现状态，不进入公共输入结果结构体。 */
typedef struct
{
    uint8_t stable_connected;
    uint16_t pending_count;
} app_input_debounce_state_t;

static app_input_debounce_state_t s_mcu_det_load_state;
static app_input_debounce_state_t s_mcu_detd_state;
static app_input_debounce_state_t s_mcu_detc_state;
static volatile uint8_t s_mcu_det_load_rearm_pending;
static uint8_t s_mcu_sw_pressed;
static uint8_t s_mcu_sw_present;
static uint16_t s_mcu_sw_debounce_count;
static uint16_t s_mcu_sw_release_count;
static uint32_t s_mcu_key_last_rx_tick;
static uint8_t s_mcu_key_frame_seen;

volatile app_input_signals_t gt_app_input_signals;

static uint8_t app_input_signals_debounce_update(
    app_input_debounce_state_t *ptState,
    uint8_t u8RawLevel,
    uint8_t u8RemovedLevel)
{
    uint8_t u8LogicalConnected;

    u8RawLevel = (u8RawLevel != 0U) ? 1U : 0U;
    u8LogicalConnected = (u8RawLevel == u8RemovedLevel) ? 0U : 1U;

    if (u8LogicalConnected == ptState->stable_connected)
    {
        ptState->pending_count = 0U;
        return ptState->stable_connected;
    }

    if (ptState->pending_count < APP_INPUT_SIGNAL_DEBOUNCE_COUNT)
    {
        ptState->pending_count++;
    }

    if (ptState->pending_count >= APP_INPUT_SIGNAL_DEBOUNCE_COUNT)
    {
        ptState->stable_connected = u8LogicalConnected;
        ptState->pending_count = 0U;
    }

    return ptState->stable_connected;
}

static void app_input_signals_update_mcu_sw(void)
{
    uint8_t u8McuSwRawLevel;
    uint8_t u8McuSwPressed;

    u8McuSwRawLevel = board_io_mcu_sw_level();
    u8McuSwRawLevel = (u8McuSwRawLevel != 0U) ? 1U : 0U;
    u8McuSwPressed =
        (u8McuSwRawLevel == APP_INPUT_SIGNAL_MCU_SW_PRESSED_LEVEL) ? 1U : 0U;

    if (u8McuSwPressed != s_mcu_sw_pressed)
    {
        if (s_mcu_sw_debounce_count < APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_COUNT)
        {
            s_mcu_sw_debounce_count++;
        }

        if (s_mcu_sw_debounce_count >= APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_COUNT)
        {
            s_mcu_sw_pressed = u8McuSwPressed;
            s_mcu_sw_debounce_count = 0U;
        }
    }
    else
    {
        s_mcu_sw_debounce_count = 0U;
    }

    if (s_mcu_sw_pressed != 0U)
    {
        s_mcu_sw_present = 1U;
        s_mcu_sw_release_count = 0U;
    }
    else if (s_mcu_sw_present != 0U)
    {
        if (s_mcu_sw_release_count < APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_COUNT)
        {
            s_mcu_sw_release_count++;
        }

        if (s_mcu_sw_release_count >= APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_COUNT)
        {
            s_mcu_sw_present = 0U;
            s_mcu_sw_release_count = 0U;
        }
    }

    gt_app_input_signals.mcu_sw_present = s_mcu_sw_present;
}

static void app_input_signals_update_mcu_key(void)
{
    uint32_t now_tick;
    uint32_t last_rx_tick;
    uint8_t frame_seen;

    taskENTER_CRITICAL();
    now_tick = osKernelSysTick();
    last_rx_tick = s_mcu_key_last_rx_tick;
    frame_seen = s_mcu_key_frame_seen;
    taskEXIT_CRITICAL();

    gt_app_input_signals.mcu_key_present =
        ((frame_seen != 0U) &&
         ((uint32_t)(now_tick - last_rx_tick) < APP_INPUT_SIGNAL_CAN_KEY_TIMEOUT_TICKS))
            ? 1U : 0U;
}

void app_input_signals_init(void)
{
    memset((void *)&gt_app_input_signals, 0, sizeof(gt_app_input_signals));
    memset(&s_mcu_det_load_state, 0, sizeof(s_mcu_det_load_state));
    memset(&s_mcu_detd_state, 0, sizeof(s_mcu_detd_state));
    memset(&s_mcu_detc_state, 0, sizeof(s_mcu_detc_state));
    s_mcu_det_load_rearm_pending = 0U;
    s_mcu_sw_pressed = 0U;
    s_mcu_sw_present = 0U;
    s_mcu_sw_debounce_count = 0U;
    s_mcu_sw_release_count = 0U;
    s_mcu_key_last_rx_tick = 0U;
    s_mcu_key_frame_seen = 0U;
}

void app_input_signals_note_can_key_frame(void)
{
    taskENTER_CRITICAL();
    s_mcu_key_last_rx_tick = osKernelSysTick();
    s_mcu_key_frame_seen = 1U;
    taskEXIT_CRITICAL();
}

void app_input_signals_load_detect_rearm(void)
{
    /* 先请求采样任务重置，再立即发布保守状态，避免BMS任务读到旧的“已移除”。 */
    s_mcu_det_load_rearm_pending = 1U;
    gt_app_input_signals.mcu_det_load = 1U;
}

void app_input_signals_update(void)
{
    uint8_t u8McuDetLoad;

    if (board_io_load_detect_enabled() == 0U)
    {
        /* 检测电路关闭时PE9的电平无效；保守地维持在位，且不累计移除消抖。 */
        s_mcu_det_load_state.stable_connected = 1U;
        s_mcu_det_load_state.pending_count = 0U;
        gt_app_input_signals.mcu_det_load = 1U;
    }
    else
    {
        if (s_mcu_det_load_rearm_pending != 0U)
        {
            s_mcu_det_load_state.stable_connected = 1U;
            s_mcu_det_load_state.pending_count = 0U;
            s_mcu_det_load_rearm_pending = 0U;
        }

        u8McuDetLoad = app_input_signals_debounce_update(
            &s_mcu_det_load_state,
            board_io_load_detect_level(),
            APP_INPUT_SIGNAL_MCU_DET_LOAD_REMOVED_LEVEL);
        gt_app_input_signals.mcu_det_load = u8McuDetLoad;
    }

    /* 若BMS任务在本次采样中请求重启，确保本次旧状态不会覆盖保守的在位状态。 */
    if (s_mcu_det_load_rearm_pending != 0U)
    {
        s_mcu_det_load_state.stable_connected = 1U;
        s_mcu_det_load_state.pending_count = 0U;
        s_mcu_det_load_rearm_pending = 0U;
        gt_app_input_signals.mcu_det_load = 1U;
    }

    gt_app_input_signals.mcu_detd = app_input_signals_debounce_update(
        &s_mcu_detd_state,
        board_io_mcu_detd_level(),
        APP_INPUT_SIGNAL_MCU_DETD_REMOVED_LEVEL);
    gt_app_input_signals.mcu_detc = app_input_signals_debounce_update(
        &s_mcu_detc_state,
        board_io_mcu_detc_level(),
        APP_INPUT_SIGNAL_MCU_DETC_REMOVED_LEVEL);
    gt_app_input_signals.mcu_2nd_det =
        (board_io_mcu_2nd_det_level() == 0U) ? 1U : 0U;

    app_input_signals_update_mcu_sw();
    app_input_signals_update_mcu_key();
}
