/*
 * 文件作用：
 *   管理电池SOC对应的LD1~LD5显示。
 *
 * 显示规则：
 *   按20%一档从LED5向LED1递增；非充电普通显示时低于10%闪烁最低一格。
 *   充电状态显示充电动画；DETC在位但未充电时显示常亮SOC。
 *   DETC不在位且未充电时，按键在位才显示SOC；上电时由输入模块提供一次默认按键窗口。
 *   充电未满时下一格以500ms亮灭交替闪烁，SOC=100%时全部常亮。
 *
 * 说明：
 *   本模块不处理心跳LED，不调用board_io_led_toggle()；心跳LED仍由原有任务保留。
 */
#include "App/app_led.h"
#include "App/app_bms_snapshot.h"
#include "App/app_bms_status.h"
#include "App/app_input_signals.h"
#include "Board/board_io.h"
#include "Driver/d_timer.h"

#define APP_LED_FAULT_CODE_DISPLAY_MS       3000UL
#define APP_LED_POST_FAULT_SOC_DISPLAY_MS   8000UL
#define APP_LED_FAULT_CODE_COUNT            31U
#define APP_LED_FAULT_FLASH_PERIOD_MS       200UL
#define APP_LED_FAULT_FLASH_ON_MS           100UL

static uint16_t s_u16AppLedBlinkCount;
static uint8_t s_u8AppLedBlinkOn;
static uint16_t s_u16AppLedLowSocBlinkCount;
static uint8_t s_u8AppLedLowSocBlinkOn;
static uint8_t s_fault_codes[APP_LED_FAULT_CODE_COUNT];
static uint8_t s_fault_code_count;
static uint8_t s_fault_code_index;
static uint8_t s_fault_display_active;
static uint8_t s_post_fault_soc_display_active;
static uint32_t s_fault_code_start_ms;
static uint32_t s_post_fault_soc_start_ms;

static uint8_t app_led_collect_fault_codes(const app_bms_snapshot_t *snapshot)
{
    const t_bms_fault_flag *fault = &snapshot->fault.active.detail;
    const t_charge_fault_A *charge_a = &fault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *charge_b = &fault->ucharge_fault_B.tcharge_fault_B;
    const t_discharge_fault_A *discharge_a = &fault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *discharge_b = &fault->udischarge_fault_B.tdischarge_fault_B;
    const t_general_fault_A *general_a = &fault->ugeneral_fault_A.tgeneral_fault_A;
    uint8_t count = 0U;

    /* 故障优先级：通用、放电、充电；仅列故障/保护位，排除告警和满充标志。 */
#define APP_LED_ADD_FAULT(code, condition) \
    do { \
        if (((condition) != 0U) && (count < APP_LED_FAULT_CODE_COUNT)) \
        { \
            s_fault_codes[count++] = (uint8_t)(code); \
        } \
    } while (0)

    /* 通用故障：1~11；告警位不显示。 */
    APP_LED_ADD_FAULT(1U, general_a->b1AFE_fault);
    APP_LED_ADD_FAULT(2U, general_a->b1cell_voltage_diff_fault);
    APP_LED_ADD_FAULT(3U, general_a->b1CHG_MOS_failure);
    APP_LED_ADD_FAULT(4U, general_a->b1DSG_MOS_faulure);
    APP_LED_ADD_FAULT(5U, general_a->b1MOS_OTP);
    APP_LED_ADD_FAULT(6U, general_a->b1hardware_second_pro);
    APP_LED_ADD_FAULT(7U, general_a->b1SCP_FUSE_fusing);
    APP_LED_ADD_FAULT(8U, general_a->b1MOS_OTP_FUSE);
    APP_LED_ADD_FAULT(9U, general_a->b1OTD_FUSE);
    APP_LED_ADD_FAULT(10U, general_a->b1NTC_fault);
    APP_LED_ADD_FAULT(11U, general_a->b1cell_voltage_diff_fault2);

    /* 放电故障：12~25；分级欠压/过流各自使用独立编号。 */
    APP_LED_ADD_FAULT(12U, discharge_a->b1cell_pro_cuv1);
    APP_LED_ADD_FAULT(13U, discharge_a->b1cell_deep_cuv);
    APP_LED_ADD_FAULT(14U, discharge_a->b1pack_pro_otd);
    APP_LED_ADD_FAULT(15U, discharge_a->b1pack_pro_utd);
    APP_LED_ADD_FAULT(16U, discharge_a->b1pack_ocd_lock);
    APP_LED_ADD_FAULT(17U, discharge_a->b1cell_pro_cuv2);
    APP_LED_ADD_FAULT(18U, discharge_b->b1pack_pro_ocd1);
    APP_LED_ADD_FAULT(19U, discharge_b->b1pack_pro_cuv);
    APP_LED_ADD_FAULT(20U, discharge_b->b1pack_pro_ocd2);
    APP_LED_ADD_FAULT(21U, discharge_b->b1pack_pro_ocd3);
    APP_LED_ADD_FAULT(22U, discharge_b->b1pack_pro_ocd4);
    APP_LED_ADD_FAULT(23U, discharge_b->b1pack_shrot);
    APP_LED_ADD_FAULT(24U, discharge_b->b1predischarge_fault);
    APP_LED_ADD_FAULT(25U, discharge_b->b1predischarge_lock);

    /* 充电故障：26~31；单体过压、总压过压和满充标志不显示。 */
    APP_LED_ADD_FAULT(26U, charge_a->b1pack_pro_otc);
    APP_LED_ADD_FAULT(27U, charge_a->b1pack_pro_utc);
    APP_LED_ADD_FAULT(28U, charge_a->b1pack_pro_occ1);
    APP_LED_ADD_FAULT(29U, charge_a->b1pack_pro_orcc);
    APP_LED_ADD_FAULT(30U, charge_a->b1pack_pro_occ2);
    APP_LED_ADD_FAULT(31U, charge_b->b1pack_occ_lock);

#undef APP_LED_ADD_FAULT
    return count;
}

static uint8_t app_led_fault_trit_to_level(uint8_t trit, uint32_t now_ms)
{
    if (trit == 0U)
    {
        return APP_LED_OFF_LEVEL;
    }

    if (trit == 1U)
    {
        return APP_LED_ON_LEVEL;
    }

    /* 三态编码的2表示5 Hz频闪：100 ms亮、100 ms灭。 */
    return (((now_ms % APP_LED_FAULT_FLASH_PERIOD_MS) < APP_LED_FAULT_FLASH_ON_MS) ?
            APP_LED_ON_LEVEL : APP_LED_OFF_LEVEL);
}

static void app_led_display_fault_code(uint8_t code, uint32_t now_ms)
{
    uint8_t encoded_code = code;
    uint8_t led4_trit;
    uint8_t led3_trit;
    uint8_t led2_trit;
    uint8_t led1_trit;

    /* LED5固定常亮；四位三进制控制LED4~LED1，跳过1000以避开正常SOC点阵。 */
    if (encoded_code >= 27U)
    {
        encoded_code++;
    }

    led4_trit = (uint8_t)(encoded_code / 27U);
    led3_trit = (uint8_t)((encoded_code / 9U) % 3U);
    led2_trit = (uint8_t)((encoded_code / 3U) % 3U);
    led1_trit = (uint8_t)(encoded_code % 3U);

    board_io_mcu_ld5_set(APP_LED_ON_LEVEL);
    board_io_mcu_ld4_set(app_led_fault_trit_to_level(led4_trit, now_ms));
    board_io_mcu_ld3_set(app_led_fault_trit_to_level(led3_trit, now_ms));
    board_io_mcu_ld2_set(app_led_fault_trit_to_level(led2_trit, now_ms));
    board_io_mcu_ld1_set(app_led_fault_trit_to_level(led1_trit, now_ms));
}

static void app_led_start_fault_sequence(const app_bms_snapshot_t *snapshot,
                                         uint32_t now_ms)
{
    s_fault_code_count = app_led_collect_fault_codes(snapshot);
    s_fault_code_index = 0U;
    s_fault_display_active = 0U;
    s_post_fault_soc_display_active = 0U;

    if (s_fault_code_count == 0U)
    {
        s_fault_display_active = 0U;
        return;
    }

    s_fault_display_active = 1U;
    s_post_fault_soc_display_active = 0U;
    s_fault_code_start_ms = now_ms;
}

static uint8_t app_led_update_fault_sequence(uint32_t now_ms)
{
    if (s_fault_display_active == 0U)
    {
        return 0U;
    }

    while ((s_fault_code_index < s_fault_code_count) &&
           ((uint32_t)(now_ms - s_fault_code_start_ms) >= APP_LED_FAULT_CODE_DISPLAY_MS))
    {
        s_fault_code_index++;
        s_fault_code_start_ms += APP_LED_FAULT_CODE_DISPLAY_MS;
    }

    if (s_fault_code_index >= s_fault_code_count)
    {
        s_fault_display_active = 0U;
        s_post_fault_soc_display_active = 1U;
        s_post_fault_soc_start_ms = s_fault_code_start_ms;
        return 0U;
    }

    app_led_display_fault_code(s_fault_codes[s_fault_code_index], now_ms);
    return 1U;
}

static uint8_t app_led_update_post_fault_soc_window(uint32_t now_ms)
{
    if ((s_post_fault_soc_display_active != 0U) &&
        ((uint32_t)(now_ms - s_post_fault_soc_start_ms) >= APP_LED_POST_FAULT_SOC_DISPLAY_MS))
    {
        s_post_fault_soc_display_active = 0U;
    }

    return s_post_fault_soc_display_active;
}

/* 故障码轮播和轮播后的SOC窗口内，低功耗必须等待显示结束。 */
uint8_t app_led_holds_awake(void)
{
    return ((s_fault_display_active != 0U) ||
            (s_post_fault_soc_display_active != 0U)) ? 1U : 0U;
}

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
    s_fault_code_count = 0U;
    s_fault_code_index = 0U;
    s_fault_display_active = 0U;
    s_post_fault_soc_display_active = 0U;
    s_fault_code_start_ms = 0U;
    s_post_fault_soc_start_ms = 0U;
    app_led_set_all(0U);
}

void app_led_task(void)
{
    app_bms_snapshot_t tAppBmsSnapshot;
    uint32_t now_ms = d_timer_get_ms();
    uint8_t post_fault_soc_window;

    app_bms_snapshot_read(&tAppBmsSnapshot);

    /* 每次真实消抖按下都启动一轮；多项故障随后每3秒自动切换。 */
    if (app_input_signals_take_mcu_sw_press_event() != 0U)
    {
        app_led_start_fault_sequence(&tAppBmsSnapshot, now_ms);
    }

    if (app_led_update_fault_sequence(now_ms) != 0U)
    {
        return;
    }
    post_fault_soc_window = app_led_update_post_fault_soc_window(now_ms);

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

    if ((tAppBmsSnapshot.signal.input.mcu_sw_present == 0U) &&
        (post_fault_soc_window == 0U))
    {
        app_led_reset_blink();
        app_led_reset_low_soc_blink();
        app_led_set_all(0U);
        return;
    }

    app_led_display_soc(tAppBmsSnapshot.soc.u8display_soc, 0U);
}
