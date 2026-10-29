/*
 * 文件作用：
 *   管理电池SOC对应的LD1~LD5显示。
 *
 * 显示规则：
 *   按20%一档从LED5向LED1递增；非充电普通显示时低于10%闪烁最低一格。
 *   充电状态显示充电动画；DETC在位但未充电时显示常亮SOC。
 *   DETC不在位且未充电时，按键在位才显示SOC。
 *   充电未满时下一格以500ms亮灭交替闪烁，SOC=100%时全部常亮。
 *
 * 说明：
 *   本模块不处理心跳LED，不调用board_io_led_toggle()；心跳LED仍由原有任务保留。
 */
#include "App/app_led.h"
#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "Board/board_io.h"

static uint16_t s_u16AppLedBlinkCount;
static uint8_t s_u8AppLedBlinkOn;
static uint16_t s_u16AppLedLowSocBlinkCount;
static uint8_t s_u8AppLedLowSocBlinkOn;

static uint8_t app_led_soc_to_count(uint8_t u8SocPercent)
{
    uint8_t u8LedCount;

    if (u8SocPercent == 0U)
    {
        return 0U;
    }

    if (u8SocPercent > APP_LED_SOC_MAX_PERCENT)
    {
        u8SocPercent = APP_LED_SOC_MAX_PERCENT;
    }

    u8LedCount = (uint8_t)((u8SocPercent + APP_LED_SOC_STEP_PERCENT - 1U) /
                           APP_LED_SOC_STEP_PERCENT);
    if (u8LedCount > APP_LED_LEVEL_COUNT)
    {
        u8LedCount = APP_LED_LEVEL_COUNT;
    }

    return u8LedCount;
}

static void app_led_set_all(uint8_t u8On)
{
    uint8_t u8Level;

    u8Level = (u8On != 0U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL;
    board_io_mcu_ld1_set(u8Level);
    board_io_mcu_ld2_set(u8Level);
    board_io_mcu_ld3_set(u8Level);
    board_io_mcu_ld4_set(u8Level);
    board_io_mcu_ld5_set(u8Level);
}

static void app_led_reset_blink(void)
{
    s_u16AppLedBlinkCount = 0U;
    s_u8AppLedBlinkOn = 1U;
}

static void app_led_reset_low_soc_blink(void)
{
    s_u16AppLedLowSocBlinkCount = 0U;
    s_u8AppLedLowSocBlinkOn = 1U;
}

static void app_led_update_blink(void)
{
    if (s_u16AppLedBlinkCount < APP_LED_BLINK_COUNT)
    {
        s_u16AppLedBlinkCount++;
    }

    if (s_u16AppLedBlinkCount >= APP_LED_BLINK_COUNT)
    {
        s_u16AppLedBlinkCount = 0U;
        s_u8AppLedBlinkOn = (s_u8AppLedBlinkOn == 0U) ? 1U : 0U;
    }
}

static void app_led_update_low_soc_blink(void)
{
    if (s_u16AppLedLowSocBlinkCount < APP_LED_LOW_SOC_BLINK_COUNT)
    {
        s_u16AppLedLowSocBlinkCount++;
    }

    if (s_u16AppLedLowSocBlinkCount >= APP_LED_LOW_SOC_BLINK_COUNT)
    {
        s_u16AppLedLowSocBlinkCount = 0U;
        s_u8AppLedLowSocBlinkOn = (s_u8AppLedLowSocBlinkOn == 0U) ? 1U : 0U;
    }
}

static uint8_t app_led_get_charging_level(uint8_t u8LedIndex,
                                          uint8_t u8StableLedCount)
{
    if (u8LedIndex <= u8StableLedCount)
    {
        return APP_LED_ON_LEVEL;
    }

    if ((u8LedIndex == (uint8_t)(u8StableLedCount + 1U)) &&
        (s_u8AppLedBlinkOn != 0U))
    {
        return APP_LED_ON_LEVEL;
    }

    return APP_LED_OFF_LEVEL;
}

static void app_led_set_charging_level(uint8_t u8StableLedCount)
{
    board_io_mcu_ld1_set(app_led_get_charging_level(5U, u8StableLedCount));
    board_io_mcu_ld2_set(app_led_get_charging_level(4U, u8StableLedCount));
    board_io_mcu_ld3_set(app_led_get_charging_level(3U, u8StableLedCount));
    board_io_mcu_ld4_set(app_led_get_charging_level(2U, u8StableLedCount));
    board_io_mcu_ld5_set(app_led_get_charging_level(1U, u8StableLedCount));
}

static void app_led_display_charging_soc(uint8_t u8SocPercent)
{
    uint8_t u8StableLedCount;

    if (u8SocPercent >= APP_LED_SOC_MAX_PERCENT)
    {
        app_led_reset_blink();
        app_led_set_all(1U);
        return;
    }

    u8StableLedCount = (uint8_t)(u8SocPercent / APP_LED_SOC_STEP_PERCENT);
    app_led_update_blink();
    app_led_set_charging_level(u8StableLedCount);
}

static void app_led_display_soc(uint8_t u8SocPercent, uint8_t u8Steady)
{
    uint8_t u8LedCount;

    if ((u8Steady == 0U) && (u8SocPercent < APP_LED_LOW_SOC_THRESHOLD_PERCENT))
    {
        app_led_reset_blink();
        app_led_update_low_soc_blink();
        board_io_mcu_ld5_set(s_u8AppLedLowSocBlinkOn != 0U ?
                             APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
        board_io_mcu_ld1_set(APP_LED_OFF_LEVEL);
        board_io_mcu_ld2_set(APP_LED_OFF_LEVEL);
        board_io_mcu_ld3_set(APP_LED_OFF_LEVEL);
        board_io_mcu_ld4_set(APP_LED_OFF_LEVEL);
        return;
    }

    app_led_reset_blink();
    app_led_reset_low_soc_blink();
    u8LedCount = app_led_soc_to_count(u8SocPercent);
    if ((u8Steady != 0U) && (u8LedCount == 0U))
    {
        /* 充电器在位且未充电时，0%也以最低一格常亮表示连接。 */
        u8LedCount = 1U;
    }
    board_io_mcu_ld1_set((u8LedCount >= 5U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
    board_io_mcu_ld2_set((u8LedCount >= 4U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
    board_io_mcu_ld3_set((u8LedCount >= 3U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
    board_io_mcu_ld4_set((u8LedCount >= 2U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
    board_io_mcu_ld5_set((u8LedCount >= 1U) ? APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
}

void app_led_init(void)
{
    app_led_reset_blink();
    app_led_reset_low_soc_blink();
    app_led_set_all(0U);
}

void app_led_task(void)
{
    app_bms_snapshot_t tAppBmsSnapshot;

    app_bms_snapshot_read(&tAppBmsSnapshot);

    if (tAppBmsSnapshot.bms_work_status == APP_BMS_STATUS_CHARGE)
    {
        app_led_reset_low_soc_blink();
        app_led_display_charging_soc(tAppBmsSnapshot.soc.u8display_soc);
        return;
    }

    if (tAppBmsSnapshot.signal.input.mcu_detc != 0U)
    {
        app_led_display_soc(tAppBmsSnapshot.soc.u8display_soc, 1U);
        return;
    }

    if (tAppBmsSnapshot.signal.input.mcu_sw_present == 0U)
    {
        app_led_reset_blink();
        app_led_reset_low_soc_blink();
        app_led_set_all(0U);
        return;
    }

    app_led_display_soc(tAppBmsSnapshot.soc.u8display_soc, 0U);
}
