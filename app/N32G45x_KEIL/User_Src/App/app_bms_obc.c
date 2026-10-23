#include "App/app_bms_obc.h"

#include <string.h>

#include "FreeRTOS.h"
#include "cmsis_os.h"
#include "task.h"

#define APP_BMS_OBC_RX_TIMEOUT_MS       (5000U)
#define APP_BMS_OBC_ALIVE_STUCK_LIMIT   (10U)
#define APP_BMS_OBC_MS_TO_TICKS(ms) \
    ((uint32_t)((((uint64_t)(ms) * (uint64_t)osKernelSysTickFrequency) + 999U) / 1000U))
#define APP_BMS_OBC_RX_TIMEOUT_TICKS \
    APP_BMS_OBC_MS_TO_TICKS(APP_BMS_OBC_RX_TIMEOUT_MS)

static app_bms_obc_state_t s_obc_state;
static uint32_t s_detc_start_tick;
static uint32_t s_last_rx_tick;
static uint8_t s_detc_active;
static uint8_t s_rx_since_detc;
static uint8_t s_alive_counter_valid;
static uint8_t s_last_alive_counter;
static uint8_t s_alive_unchanged_count;
static uint8_t s_alive_counter_fault;

static void app_bms_obc_reset_alive_counter(void)
{
    s_alive_counter_valid = 0U;
    s_last_alive_counter = 0U;
    s_alive_unchanged_count = 0U;
    s_alive_counter_fault = 0U;
}

static void app_bms_obc_update_alive_counter(uint8_t alive_counter)
{
    if (s_alive_counter_valid == 0U)
    {
        s_last_alive_counter = alive_counter;
        s_alive_counter_valid = 1U;
        s_alive_unchanged_count = 0U;
        s_alive_counter_fault = 0U;
    }
    else if (alive_counter == s_last_alive_counter)
    {
        if (s_alive_unchanged_count < APP_BMS_OBC_ALIVE_STUCK_LIMIT)
        {
            s_alive_unchanged_count++;
        }
        if (s_alive_unchanged_count >= APP_BMS_OBC_ALIVE_STUCK_LIMIT)
        {
            s_alive_counter_fault = 1U;
        }
    }
    else
    {
        s_last_alive_counter = alive_counter;
        s_alive_unchanged_count = 0U;
        s_alive_counter_fault = 0U;
    }
}

static uint8_t app_bms_obc_has_fault(const uint8_t *data)
{
    uint8_t fan_status;
    uint8_t relay_fault;

    /* Data0/1内所有非零等级或置位均为协议定义的OBC故障。 */
    if ((data[0] != 0U) || (data[1] != 0U))
    {
        return 1U;
    }

    if ((data[2] & 0x03U) != 0U)
    {
        return 1U;
    }

    fan_status = (uint8_t)((data[2] >> 2U) & 0x03U);
    relay_fault = (uint8_t)((data[2] >> 4U) & 0x03U);
    if ((fan_status == 2U) || (relay_fault == 3U) ||
        (s_alive_counter_fault != 0U))
    {
        return 1U;
    }

    return 0U;
}

static void app_bms_obc_update_status_from_frame(void)
{
    s_obc_state.status = (app_bms_obc_has_fault(s_obc_state.raw_data) != 0U) ?
                         APP_BMS_OBC_STATE_FAULT : APP_BMS_OBC_STATE_PRESENT;
}

void app_bms_obc_init(void)
{
    memset(&s_obc_state, 0, sizeof(s_obc_state));
    s_obc_state.status = APP_BMS_OBC_STATE_NOT_PRESENT;
    s_detc_start_tick = 0U;
    s_last_rx_tick = 0U;
    s_detc_active = 0U;
    s_rx_since_detc = 0U;
    app_bms_obc_reset_alive_counter();
}

void app_bms_obc_receive(const uint8_t *data, uint8_t length)
{
    uint32_t now_tick;

    if ((data == 0) || (length != APP_BMS_OBC_FRAME_SIZE))
    {
        return;
    }

    taskENTER_CRITICAL();
    now_tick = osKernelSysTick();
    memcpy(s_obc_state.raw_data, data, APP_BMS_OBC_FRAME_SIZE);
    s_obc_state.frame_valid = 1U;

    if (s_detc_active != 0U)
    {
        s_obc_state.frame_sequence++;
        s_rx_since_detc = 1U;
        s_last_rx_tick = now_tick;
        app_bms_obc_update_alive_counter((uint8_t)(data[7] & 0x0FU));
        app_bms_obc_update_status_from_frame();
    }
    taskEXIT_CRITICAL();
}

void app_bms_obc_task(uint8_t detc_present)
{
    uint32_t now_tick;

    taskENTER_CRITICAL();
    /* 在临界区内取时基，避免等待接收临界区后用旧时间误判刚到的报文超时。 */
    now_tick = osKernelSysTick();

    if (detc_present == 0U)
    {
        s_detc_active = 0U;
        s_rx_since_detc = 0U;
        app_bms_obc_reset_alive_counter();
        s_obc_state.status = APP_BMS_OBC_STATE_NOT_PRESENT;
    }
    else if (s_detc_active == 0U)
    {
        s_detc_active = 1U;
        s_detc_start_tick = now_tick;
        s_rx_since_detc = 0U;
        app_bms_obc_reset_alive_counter();
        /* DETC刚接入时先认为OBC在位，等待首帧的5秒宽限期。 */
        s_obc_state.status = APP_BMS_OBC_STATE_PRESENT;
    }
    else if (s_rx_since_detc != 0U)
    {
        if ((uint32_t)(now_tick - s_last_rx_tick) >= APP_BMS_OBC_RX_TIMEOUT_TICKS)
        {
            s_rx_since_detc = 0U;
            app_bms_obc_reset_alive_counter();
            s_obc_state.status = APP_BMS_OBC_STATE_NOT_PRESENT;
        }
        else
        {
            app_bms_obc_update_status_from_frame();
        }
    }
    else if ((uint32_t)(now_tick - s_detc_start_tick) >= APP_BMS_OBC_RX_TIMEOUT_TICKS)
    {
        s_obc_state.status = APP_BMS_OBC_STATE_NOT_PRESENT;
    }
    else
    {
        s_obc_state.status = APP_BMS_OBC_STATE_PRESENT;
    }

    taskEXIT_CRITICAL();
}

void app_bms_obc_read(app_bms_obc_state_t *state)
{
    if (state == 0)
    {
        return;
    }

    taskENTER_CRITICAL();
    *state = s_obc_state;
    taskEXIT_CRITICAL();
}
