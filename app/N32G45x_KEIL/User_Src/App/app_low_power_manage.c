/*
 * 文件作用：
 *   APP层低功耗管理模块，负责判断系统是否满足STOP休眠条件，并处理休眠前后状态。
 *
 * 主要用途：
 *   1. 在无电流、无通信并持续满足时间后请求进入低功耗。
 *   2. 配置休眠前GPIO、AFE、RTC唤醒和看门狗保持策略。
 *   3. 唤醒后恢复时钟、外设、AFE和暂停前的业务时基。
 *
 * 适用范围：
 *   适用于当前N32G45X STOP低功耗方案。后续模块化改造时，本模块应只判断休眠策略，
 *   具体硬件动作逐步收敛到硬件动作适配层。
 *
 * 调用关系：
 *   BMS_Task 10ms任务调用 app_low_power_task(10U)；本模块当前仍直接调用Board、EXTI、RTC、System和Watchdog接口。
 *
 * 注意事项：
 *   进入STOP前必须保证唤醒源配置正确；IWDG不能关闭，必须由RTC周期唤醒后单独喂狗。
 */
#include "App/app_low_power_manage.h"

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "App/app_balance_manage.h"
#include "App/app_bms_fault.h"
#include "App/app_can.h"
#include "App/app_bms_snapshot.h"
#include "App/app_power_manage.h"
#include "App/app_current_calib.h"
#include "App/app_h_power_manage.h"
#include "App/app_param.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#if (APP_PARAM_UDS_ENABLE == 1U)
#include "Protocol/UDS/UDSLogic/SID28_CommunicationControl.h"
#endif
#include "Board/board_io.h"
#include "Driver/d_adc.h"
#include "Driver/d_can.h"
#include "Driver/d_timer.h"
#include "Driver/d_uart.h"
#include "HSI_afe/hsi_afe.h"
#include "HSI_afe/hsi_afe_dvc1124.h"
#include "Driver/d_exti.h"
#include "Driver/d_gpio.h"
#include "Driver/d_rtc.h"
#include "RTT/SEGGER_RTT.h"
#include "Driver/d_system.h"
#include "Driver/d_watchdog.h"

/*
 * APP低功耗管理。
 *
 * 当前设计：
 * 1. 该模块由10ms任务调用，不在中断里做耗时动作。
 * 2. 连续满足低功耗门禁达到配置空闲时间后进入STOP0；DETC/DETD均不在位时改为请求软关机。
 * 3. STOP0期间保留RTC自动唤醒，同时允许AFE_ALERT、串口RX、CAN_WK、按键、DETD和DETC下降沿唤醒。
 * 4. 唤醒后先恢复看门狗超时复位能力，再恢复时钟、GPIO、串口、AFE等运行资源。
 * 5. RTC周期唤醒后不恢复任务调度，直接检查AFE新电流样本；零点死区后的业务电流为0时继续STOP。
 *
 * 注意：
 * 低功耗是否真正启用，取决于BMS_Task是否持续调用app_low_power_task()。
 */

/* 空闲判定时间，单位毫秒；由APP_LOW_POWER_IDLE_SECONDS配置。 */
#define APP_LOW_POWER_IDLE_MS            ((uint32_t)APP_LOW_POWER_IDLE_SECONDS * 1000UL)
#define APP_LOW_POWER_DIAG_PERIOD_MS     1000UL

/* DVC1124的CC2电流采样周期为256ms，RTC巡检时等待一轮新转换后再读取。 */
#define APP_LOW_POWER_AFE_CURRENT_WAIT_MS 260UL

/* STOP前系统时钟切换的固定超时计数，避免时钟异常时无限等待。 */
#define APP_LOW_POWER_CLOCK_WAIT_TIMEOUT 100000UL

/*
 * RTC主动唤醒周期，单位秒；必须小于IWDG低功耗最大约26S窗口。
 * 每20S唤醒一次，低功耗路径先喂狗再恢复外设，随后继续进入STOP。
 */

typedef enum
{
    APP_LOW_POWER_WAKE_NONE = 0U,        /* 没有识别到明确唤醒源，通常用于调试兜底。 */
    APP_LOW_POWER_WAKE_RTC,              /* RTC 20S周期唤醒。 */
    APP_LOW_POWER_WAKE_AFE,              /* AFE_ALERT下降沿唤醒，可能是电流或故障。 */
    APP_LOW_POWER_WAKE_UART,             /* MCU_RX下降沿唤醒，表示上位机开始通信。 */
    APP_LOW_POWER_WAKE_CAN,              /* CAN_WK下降沿唤醒。 */
    APP_LOW_POWER_WAKE_KEY,              /* MCU_SW下降沿唤醒。 */
    APP_LOW_POWER_WAKE_DETD,             /* DETD下降沿唤醒。 */
    APP_LOW_POWER_WAKE_DETC              /* DETC下降沿唤醒。 */
} app_low_power_wake_reason_t;

typedef enum
{
    APP_LOW_POWER_BLOCK_NONE = 0U,
    APP_LOW_POWER_BLOCK_SHUTDOWN,
    APP_LOW_POWER_BLOCK_IAP,
    APP_LOW_POWER_BLOCK_CAN_SESSION,
    APP_LOW_POWER_BLOCK_WAKE_EVENT,
    APP_LOW_POWER_BLOCK_CURRENT,
    APP_LOW_POWER_BLOCK_EXTERNAL_INPUT,
    APP_LOW_POWER_BLOCK_COMMUNICATION
} app_low_power_block_reason_t;

static uint32_t s_app_low_power_idle_ms;                    /* 已连续满足低功耗条件的毫秒数。 */
static uint32_t s_app_low_power_auto_shutdown_idle_ms;      /* DETC/DETD均不在位时的连续空闲关机计时。 */
static uint32_t s_app_low_power_last_rx_count;              /* 上一次10ms检查时的串口接收计数。 */
static uint32_t s_app_low_power_last_tx_count;              /* 上一次10ms检查时的串口发送计数。 */
static uint32_t s_app_low_power_last_can_rx_count;          /* 所有CAN接收帧的上次计数。 */
static uint32_t s_app_low_power_last_can_tx_count;          /* 所有CAN成功提交发送帧的上次计数。 */
static uint8_t s_app_low_power_comm_block_mask;             /* 通信门禁细分原因，供RTT诊断。 */
static uint8_t s_app_low_power_comm_check_valid;            /* 本轮是否实际执行过通信门禁检查。 */
static uint8_t s_app_low_power_comm_diag_mask;              /* 诊断周期内出现过的通信阻塞位。 */
static uint32_t s_app_low_power_block_diag_mask;            /* 诊断周期内出现过的低功耗阻塞原因位。 */
static uint32_t s_app_low_power_diag_elapsed_ms;             /* 周期性低功耗诊断输出计时。 */
static uint32_t s_app_low_power_wakeup_current_seq;         /* 唤醒前最后一次AFE电流样本。 */
static uint8_t s_app_low_power_need_fresh_current;          /* 唤醒后必须取得新样本才可回睡。 */
static volatile uint8_t s_app_low_power_afe_wakeup;         /* AFE_ALERT唤醒标志，中断回调里置位。 */
static volatile uint8_t s_app_low_power_uart_wakeup;        /* RX唤醒标志，中断回调里置位。 */
static volatile uint8_t s_app_low_power_can_wakeup;
static volatile uint8_t s_app_low_power_key_wakeup;
static volatile uint8_t s_app_low_power_detd_wakeup;
static volatile uint8_t s_app_low_power_detc_wakeup;
static uint8_t s_app_low_power_can_suspended;
static uint8_t s_app_low_power_adc_suspended;
static uint8_t s_app_low_power_timer_suspended;
static uint8_t s_app_low_power_uart_suspended;
static volatile uint8_t s_app_low_power_shutdown_inhibit;   /* 关机期间保持MCU运行，不进入STOP。 */
static volatile uint8_t s_app_low_power_stage;               /* 低功耗事务阶段，供Watch定位复位发生位置。 */
static volatile uint8_t s_app_low_power_sleep_flag;          /* 1=已布置休眠并即将执行WFI，0=普通运行/已返回。 */
static app_low_power_block_reason_t s_app_low_power_block_reason;
static app_low_power_wake_reason_t s_app_low_power_wake_reason; /* 最近一次唤醒原因，便于调试观察。 */
static app_h_power_manage_output_t s_app_low_power_mos_output; /* STOP前的MOS输出镜像。 */

static void app_low_power_set_stage(uint8_t stage, const char *name)
{
    (void)name;
    s_app_low_power_stage = stage;
}

static void app_low_power_afe_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;

    /*
     * AFE_ALERT下降沿只记录标志。
     * 真实AFE状态读取仍交给hsi_afe_service()/hsi_afe_task()，避免中断里走IIC导致阻塞。
     */
    s_app_low_power_afe_wakeup = 1U;
    gt_hsi_afe.alert_pending = 1U; /* 保留AFE模块原来的ALERT消费路径。 */
}

static void app_low_power_uart_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;

    /*
     * 串口RX下降沿通常是起始位。
     * 这里不直接接收数据，只负责把CPU从STOP唤醒；USART恢复后再按正常串口中断接收后续数据。
     */
    s_app_low_power_uart_wakeup = 1U;
}

static void app_low_power_can_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;
    s_app_low_power_can_wakeup = 1U;
    /* CAN_WK按一次性唤醒源处理，避免低电平抖动/重复边沿反复进入中断。 */
    EXTI->IMASK &= ~EXTI_LINE8;
}

static void app_low_power_key_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;
    s_app_low_power_key_wakeup = 1U;
}

static void app_low_power_detd_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;
    s_app_low_power_detd_wakeup = 1U;
}

static void app_low_power_detc_wakeup_callback(void *object, uint8_t bit)
{
    (void)object;
    (void)bit;
    s_app_low_power_detc_wakeup = 1U;
}

static bool app_low_power_is_current_idle(void)
{
    app_bms_snapshot_t snapshot;

    app_bms_snapshot_read(&snapshot);
    if ((snapshot.data_ready == 0U) ||
        (snapshot.afe_status_valid == 0U) ||
        (snapshot.afe_current_update_seq == 0UL))
    {
        return false;
    }

    if (s_app_low_power_need_fresh_current != 0U)
    {
        if (snapshot.afe_current_update_seq == s_app_low_power_wakeup_current_seq)
        {
            return false;
        }
        s_app_low_power_need_fresh_current = 0U;
    }

    /* 使用300mA零点死区后的业务电流，不再维护第二套低功耗电流门限。 */
    return (snapshot.current_10ma == 0);
}

/*
 * RTC唤醒后在不恢复业务调度的情况下读取AFE新电流样本。
 * 第一次I2C访问负责唤醒DVC1124；只临时启动TIM6/7等待CC2转换，再次关停后读取校准值。
 * CAN、ADC、UART和FreeRTOS调度器在整个检查期间保持关闭/暂停。
 */
static bool app_low_power_read_rtc_current(int32_t *current_ma)
{
    int32_t raw_current_ma;
    int32_t current_register;
    int32_t calibrated_current_ma;
    int32_t deadband_ma;
    uint32_t current_k_ppm;
    int32_t current_b_ma;

    if ((current_ma == 0) || (gt_hsi_afe.i2c == 0))
    {
        return false;
    }

    /* 此次访问会把AFE从Sleep唤醒；第一次读数可能仍是旧缓存，不参与门限判断。 */
    (void)hsi_afe_dvc1124_CalcCurrent(gt_hsi_afe.i2c, &raw_current_ma, &current_register);

    /* 仅恢复硬件毫秒计时等待一轮CC2新转换，不恢复FreeRTOS调度。 */
    d_timer_reinit_after_stop();
    d_timer_delay_ms(5UL);
    /* AFE从Sleep转Normal会输出一次1ms唤醒脉冲，消费本次主动唤醒对应的标志。 */
    s_app_low_power_afe_wakeup = 0U;
    gt_hsi_afe.alert_pending = 0U;
    d_timer_delay_ms(APP_LOW_POWER_AFE_CURRENT_WAIT_MS - 5UL);
    d_timer_prepare_stop();

    if (!hsi_afe_dvc1124_CalcCurrent(gt_hsi_afe.i2c, &raw_current_ma, &current_register) ||
        !app_current_calib_get(&current_k_ppm, &current_b_ma))
    {
        return false;
    }

    calibrated_current_ma = app_current_calib_calculate_output(current_k_ppm,
                                                               current_b_ma,
                                                               raw_current_ma);
    gt_hsi_afe.curr_raw = raw_current_ma;
    gt_hsi_afe.curr_reg = current_register;
    gt_hsi_afe.curr_calibrated = calibrated_current_ma;
    gt_hsi_afe.curr_real = calibrated_current_ma;
    deadband_ma = (int32_t)APP_PARAM_CURRENT_ZERO_DEADBAND_MA;
    if ((deadband_ma > 0) &&
        (gt_hsi_afe.curr_real > -deadband_ma) &&
        (gt_hsi_afe.curr_real < deadband_ma))
    {
        gt_hsi_afe.curr_real = 0;
    }
    gt_hsi_afe.current_update_seq++;
    /* RTC回睡判断复用零点死区后的业务电流，归零才允许继续STOP。 */
    *current_ma = gt_hsi_afe.curr_real;
    return true;
}

static bool app_low_power_has_non_rtc_wakeup(void)
{
    return ((s_app_low_power_afe_wakeup != 0U) ||
            (s_app_low_power_uart_wakeup != 0U) ||
            (s_app_low_power_can_wakeup != 0U) ||
            (s_app_low_power_key_wakeup != 0U) ||
            (s_app_low_power_detd_wakeup != 0U) ||
            (s_app_low_power_detc_wakeup != 0U) ||
            (board_io_can_wk_level() == 0U));
}

static bool app_low_power_is_comm_idle(void)
{
    uint32_t u32UartRxCount = d_uart_get_rx_count();
    uint32_t u32UartTxCount = d_uart_get_tx_count();
    uint32_t u32CanRxCount = d_can_get_rx_activity_count();
    uint32_t u32CanTxCount = d_can_get_tx_activity_count();
    uint8_t u8CommBlockMask = 0U;
    uint8_t u8UartTxPending = (d_uart_tx_pending() != 0U) ? 1U : 0U;
    uint8_t u8CanRxIdle = d_can_is_rx_idle(&can0) ? 1U : 0U;
    uint8_t u8CanTxIdle = d_can_is_tx_idle(&can0) ? 1U : 0U;

    s_app_low_power_comm_check_valid = 1U;

    /*
     * 串口空闲判断：
     * 1. 本次10ms周期内RX计数没有变化，说明没有收到新数据。
     * 2. 本次10ms周期内TX计数没有变化，说明没有新的发送动作。
     * 3. 发送缓存为空，说明最后一帧已经发完。
     */
    if (u32UartRxCount != s_app_low_power_last_rx_count) u8CommBlockMask |= 0x01U;
    if (u32UartTxCount != s_app_low_power_last_tx_count) u8CommBlockMask |= 0x02U;
    if (u8UartTxPending != 0U) u8CommBlockMask |= 0x04U;
    if (u32CanRxCount != s_app_low_power_last_can_rx_count) u8CommBlockMask |= 0x08U;
    if (u32CanTxCount != s_app_low_power_last_can_tx_count) u8CommBlockMask |= 0x10U;
    if (u8CanRxIdle == 0U) u8CommBlockMask |= 0x20U;
    if (u8CanTxIdle == 0U) u8CommBlockMask |= 0x40U;
    s_app_low_power_comm_block_mask = u8CommBlockMask;
    s_app_low_power_comm_diag_mask |= u8CommBlockMask;

    /* 保存本次计数，给下一次10ms检查做差分比较。 */
    s_app_low_power_last_rx_count = u32UartRxCount;
    s_app_low_power_last_tx_count = u32UartTxCount;
    s_app_low_power_last_can_rx_count = u32CanRxCount;
    s_app_low_power_last_can_tx_count = u32CanTxCount;

    return (u8CommBlockMask == 0U);
}

static bool app_low_power_is_external_input_idle(void)
{
    app_bms_snapshot_t tAppBmsSnapshot;

    app_bms_snapshot_read(&tAppBmsSnapshot);

    /* 按键在位或5秒保持窗口未结束时，不允许进入STOP。 */
    if (tAppBmsSnapshot.signal.input.mcu_sw_present != 0U)
    {
        return false;
    }

    return true;
}

static bool app_low_power_is_sleep_allowed(void)
{
    if (s_app_low_power_shutdown_inhibit != 0U)
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_SHUTDOWN;
        return false;
    }

    /*
     * 升级流程中禁止低功耗。
     * 原因：IAP收包/擦写期间如果进入STOP，会导致升级中断或Flash流程被打断。
     */
    if (ducar_iap_is_busy())
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_IAP;
        return false;
    }

    /* 宗申主动CAN会话结束前保持运行；会话超时后DETC本身不再永久阻止休眠。 */
    if (!app_can_is_sleep_allowed())
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_CAN_SESSION;
        return false;
    }

    /* 低功耗只在没有明显充放电电流时允许进入。 */
    if (!app_low_power_is_current_idle())
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_CURRENT;
        return false;
    }

    /* 按键在位和充电接入都是外部有效状态，任一成立都禁止休眠。 */
    if (!app_low_power_is_external_input_idle())
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_EXTERNAL_INPUT;
        return false;
    }

    /* 判断UART是否空闲；通信不空闲时继续保持运行。 */
    if (!app_low_power_is_comm_idle())
    {
        s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_COMMUNICATION;
        return false;
    }

    s_app_low_power_block_reason = APP_LOW_POWER_BLOCK_NONE;
    return true;
}

void app_low_power_set_shutdown_inhibit(uint8_t inhibit)
{
    s_app_low_power_shutdown_inhibit = (inhibit != 0U) ? 1U : 0U;
}

static void app_low_power_reset_idle_state(void)
{
    /*
     * 清除低功耗计时状态。
     * 进入STOP失败、出现电流/通信/升级动作时，都必须重新累计配置的空闲时间。
     */
    s_app_low_power_idle_ms = 0U;
    s_app_low_power_auto_shutdown_idle_ms = 0U;
}

static uint32_t app_low_power_target_idle_ms(void)
{
    /* 宗申报文无电流超时后，协议已停止周期发送，跳过额外的通用60秒等待。 */
    if (app_can_is_zs_idle_timeout_complete())
    {
        return 0U;
    }
    return APP_LOW_POWER_IDLE_MS;
}

static void app_low_power_prepare_gpio(void)
{
    /*
     * AFE_COFF/AFE_DOFF为低有效硬线控制。
     * 进入STOP前保持高电平释放，避免MCU休眠时误关AFE控制的充放电路径。
     */
    d_gpio_set_state(BOARD_IO_AFE_COFF_PORT, BOARD_IO_AFE_COFF_PIN, 1U);
    d_gpio_set_state(BOARD_IO_AFE_DOFF_PORT, BOARD_IO_AFE_DOFF_PIN, 1U);

    /*
     * PA5在原理图中是MCU_NTC_MOS，不是LED。
     * 低功耗前保持模拟输入，避免数字输入缓冲和上下拉带来额外漏电。
     */
    d_gpio_init(BOARD_IO_MCU_NTC_MOS_PORT, BOARD_IO_MCU_NTC_MOS_PIN, d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
}

static void app_low_power_restore_modules(void)
{
    app_low_power_set_stage(20U, "restore-clock");
    /* STOP唤醒后默认回到HSI，先恢复项目默认的HSE PLL 144MHz系统时钟。 */
    d_system_restore_clock();

    /* 上电GPIO初始化只在启动时执行；唤醒只恢复本流程中关闭的外设与电源。 */

    if (s_app_low_power_timer_suspended != 0U)
    {
        app_low_power_set_stage(21U, "restore-timers");
        taskENTER_CRITICAL();
        d_timer_reinit_after_stop();
        taskEXIT_CRITICAL();
        s_app_low_power_timer_suspended = 0U;
    }

    /* 唤醒时恢复低功耗前关闭的ADC和CAN；此时调度器仍暂停。 */
    if (s_app_low_power_adc_suspended != 0U)
    {
        app_low_power_set_stage(22U, "restore-adc");
        board_io_mcu_ntc_enable(1U);
        d_adc_resume();
        s_app_low_power_adc_suspended = 0U;
    }
    if (s_app_low_power_can_suspended != 0U)
    {
        app_low_power_set_stage(23U, "restore-can-power");
        /* 必须先恢复PC9的CAN/ISO5V_EN，再初始化CAN控制器和PA11/PA12复用。 */
        board_io_can_power_enable(1U);
        d_timer_delay_ms(10UL); /* 等待隔离CAN侧5V稳定后再启用收发器。 */
        if (!d_can_init(&can0))
        {
            board_io_can_power_enable(0U);
            SEGGER_RTT_WriteString(0, "CAN resume failed\r\n");
        }
        s_app_low_power_can_suspended = 0U;
    }

    if (s_app_low_power_uart_suspended != 0U)
    {
        app_low_power_set_stage(24U, "restore-uart");
        /* 恢复USART2 PB4/PB5复用和接收中断，后续通信回到正常串口中断流程。 */
        d_uart_init();
        s_app_low_power_uart_suspended = 0U;
    }

    /*
     * d_uart_init会清空串口环形缓存并把收发计数重新置零。
     * 唤醒前的旧快照不能继续参与比较，否则第一次10ms检查必然被误判为有通信，
     * RTC唤醒后的1S快速回睡路径就永远不会生效。
     */
    s_app_low_power_last_rx_count = d_uart_get_rx_count();
    s_app_low_power_last_tx_count = d_uart_get_tx_count();
    s_app_low_power_last_can_rx_count = d_can_get_rx_activity_count();
    s_app_low_power_last_can_tx_count = d_can_get_tx_activity_count();

    /* 恢复AFE软件I2C引脚，后续AFE_Task的10ms周期继续刷新AFE电压、电流、温度和状态。 */
    app_low_power_set_stage(25U, "restore-afe");
    if (!hsi_afe_normal(&gt_hsi_afe))
    {
        SEGGER_RTT_WriteString(0, "AFE resume interface failed\r\n");
    }

    /* 发布AFE已失效的状态，防止唤醒恢复流程继续使用休眠前的有效快照。 */
    app_bms_snapshot_update();
    s_app_low_power_wakeup_current_seq = gt_hsi_afe.current_update_seq;
    s_app_low_power_need_fresh_current = 1U;
}

static void app_low_power_config_wakeup_exti(void)
{
    /*
     * AFE_ALERT作为电流/故障类被动唤醒源。
     * 原理图上AFE_ALERT触发后产生下降沿，这里临时配置为EXTI唤醒STOP。
     */
    d_exti_open(BOARD_IO_AFE_ALERT_PORT,
                BOARD_IO_AFE_ALERT_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_NOPULL,
                app_low_power_afe_wakeup_callback,
                0);

    /*
     * MCU_RX作为通信被动唤醒源。
     * STOP中USART本身不工作，用RX引脚下降沿捕获上位机发来的起始位。
     */
    d_exti_open(BOARD_IO_MCU_RX_PORT,
                BOARD_IO_MCU_RX_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_PULLUP,
                app_low_power_uart_wakeup_callback,
                0);

    /* 参考STOP例程：CAN_WK在CAN控制器和隔离5V关闭前先挂下降沿唤醒。 */
    d_exti_open(BOARD_IO_CAN_WK_PORT,
                BOARD_IO_CAN_WK_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_PULLUP,
                app_low_power_can_wakeup_callback,
                0);

    /* 外部按键、DETD、DETC均按需求使用下降沿唤醒。 */
    d_exti_open(BOARD_IO_MCU_SW_PORT,
                BOARD_IO_MCU_SW_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_NOPULL,
                app_low_power_key_wakeup_callback,
                0);
    d_exti_open(BOARD_IO_MCU_DETD_PORT,
                BOARD_IO_MCU_DETD_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_NOPULL,
                app_low_power_detd_wakeup_callback,
                0);
    d_exti_open(BOARD_IO_MCU_DETC_PORT,
                BOARD_IO_MCU_DETC_PIN,
                d_GPIO_MODE_IT_FALLING,
                d_GPIO_NOPULL,
                app_low_power_detc_wakeup_callback,
                0);
}

static void app_low_power_close_wakeup_exti(void)
{
    /* 退出STOP后恢复正常外设功能前，关闭临时唤醒EXTI，避免和普通GPIO/USART功能冲突。 */
    d_exti_close(BOARD_IO_AFE_ALERT_PIN);
    d_exti_close(BOARD_IO_MCU_RX_PIN);
    d_exti_close(BOARD_IO_CAN_WK_PIN);
    d_exti_close(BOARD_IO_MCU_SW_PIN);
    d_exti_close(BOARD_IO_MCU_DETD_PIN);
    d_exti_close(BOARD_IO_MCU_DETC_PIN);

    /* d_exti_close只撤销EXTI映射；立即把CAN_WK恢复为普通GPIO输入。 */
    d_gpio_init(BOARD_IO_CAN_WK_PORT,
                BOARD_IO_CAN_WK_PIN,
                d_GPIO_MODE_INPUT,
                d_GPIO_NOPULL);
}

static void app_low_power_enter_stop0(void)
{
    uint32_t u32PrimaskState;
    uint32_t u32SysTickCtrl = 0UL;
    uint8_t u8CanWkLevelBeforeWfi = 1U;
    uint8_t u8CanWkExtiPendingBeforeWfi = 0U;
    uint8_t bAfeAcquired = 0U;
    uint8_t bAfeSleeping = 0U;
    uint8_t bSchedulerSuspended = 0U;
    uint8_t bWakeExtiConfigured = 0U;
    uint8_t bRtcStarted = 0U;
    uint8_t bWatchdogLowPower = 0U;
    uint8_t bSysTickStopped = 0U;
    uint8_t bDidEnterStop = 0U;

    /* 每次进入STOP前先清本轮唤醒标志，避免上一次唤醒原因影响本次判断。 */
    s_app_low_power_afe_wakeup = 0U;
    s_app_low_power_uart_wakeup = 0U;
    s_app_low_power_can_wakeup = 0U;
    s_app_low_power_key_wakeup = 0U;
    s_app_low_power_detd_wakeup = 0U;
    s_app_low_power_detc_wakeup = 0U;
    s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_NONE;
    d_rtc_wakeup_clear_pending();
    app_low_power_set_stage(1U, "prepare-start");

    /* 任务判空后若又出现收发，或接收队列/发送邮箱未空，不关闭CAN。 */
    if (!d_can_is_tx_idle(&can0) ||
        !d_can_is_rx_idle(&can0) ||
        (d_can_get_rx_activity_count() != s_app_low_power_last_can_rx_count) ||
        (d_can_get_tx_activity_count() != s_app_low_power_last_can_tx_count))
    {
        app_low_power_reset_idle_state();
        return;
    }

    /* 确保最后一帧已经发完，避免睡眠打断通信应答。 */
    if (!d_uart_wait_tx_complete())
    {
        app_low_power_reset_idle_state();
        return;
    }
    /* BMS任务拥有整段STOP切换流程的AFE资源，防止AFE任务在低功耗切换中插入访问。 */
    if (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
    {
        SEGGER_RTT_WriteString(0, "LP abort: AFE lock timeout\r\n");
        app_low_power_reset_idle_state();
        return;
    }
    bAfeAcquired = 1U;

    /* 先保存当前MOS业务输出；唤醒后由高压管理器重新经过故障门控再下发。 */
    app_h_power_manage_prepare_sleep(&s_app_low_power_mos_output);

    /* 进入STOP前先让AFE进入低功耗状态，唤醒后由hsi_afe_normal恢复。 */
    if (!hsi_afe_lowpower(&gt_hsi_afe))
    {
        SEGGER_RTT_WriteString(0, "AFE sleep command failed\r\n");
        app_low_power_reset_idle_state();
        hsi_afe_release();
        return;
    }
    bAfeSleeping = 1U;

    app_low_power_set_stage(2U, "afe-sleep-done");
    app_low_power_prepare_gpio();

    /* ADC暂停可能等待正在进行的转换，必须在暂停调度器和关闭TIM6/TIM7之前完成。 */
    d_adc_suspend();
    s_app_low_power_adc_suspended = 1U;
    app_low_power_set_stage(3U, "adc-off");

    /* 最后再确认UART发送已经排空；此时TIM6仍运行，可安全使用毫秒超时。 */
    if (!d_uart_wait_tx_complete())
    {
        goto low_power_abort;
    }

    /* 先暂停任务并停止串口；TIM6保留到CAN隔离5V关断延时完成后再停止。 */
    vTaskSuspendAll();
    bSchedulerSuspended = 1U;
    /* LD1~LD5低电平点亮；休眠前置高电平关闭。 */
    board_io_mcu_ld1_set(1U);
    board_io_mcu_ld2_set(1U);
    board_io_mcu_ld3_set(1U);
    board_io_mcu_ld4_set(1U);
    board_io_mcu_ld5_set(1U);
    d_gpio_set_state(BOARD_IO_LED_PORT, BOARD_IO_LED_PIN, BOARD_IO_LED_OFF_LEVEL);
    board_io_mcu_ntc_enable(0U);

    d_uart_prepare_stop();
    s_app_low_power_uart_suspended = 1U;
    app_low_power_set_stage(4U, "uart-off");

    /* 所有外部唤醒源先挂好，再关闭CAN控制器和隔离5V；最终入睡前统一复核事件。 */
    app_low_power_config_wakeup_exti();
    bWakeExtiConfigured = 1U;
    app_low_power_set_stage(5U, "wake-exti-armed");

    u32PrimaskState = __get_PRIMASK();
    __disable_irq();
    if ((s_app_low_power_afe_wakeup != 0U) ||
        (s_app_low_power_uart_wakeup != 0U) ||
        (s_app_low_power_can_wakeup != 0U) ||
        (s_app_low_power_key_wakeup != 0U) ||
        (s_app_low_power_detd_wakeup != 0U) ||
        (s_app_low_power_detc_wakeup != 0U) ||
        (d_can_get_rx_activity_count() != s_app_low_power_last_can_rx_count) ||
        (d_can_get_tx_activity_count() != s_app_low_power_last_can_tx_count) ||
        !d_can_is_rx_idle(&can0) ||
        !d_can_is_tx_idle(&can0))
    {
        __set_PRIMASK(u32PrimaskState);
        goto low_power_abort;
    }

    d_can_prepare_low_power(&can0);
    s_app_low_power_can_suspended = 1U;
    board_io_can_power_enable(0U);
    __set_PRIMASK(u32PrimaskState);
    /* TIM6仍运行，用10ms延时等待ISO5V关断稳定后再释放TX/RX为模拟IO。 */
    d_timer_delay_ms(10UL);
    d_can_set_low_power_pins();
    app_low_power_set_stage(6U, "can-iso5v-off");

    d_timer_prepare_stop();
    s_app_low_power_timer_suspended = 1U;
    app_low_power_set_stage(7U, "tim6-tim7-off");

    /* 所有高功耗外设关闭后才启动RTC 20秒唤醒，避免RTC先跑掉部分看门狗窗口。 */
    if (!d_rtc_wakeup_start(APP_LOW_POWER_RTC_WAKE_SECONDS))
    {
        SEGGER_RTT_WriteString(0, "LP abort: RTC wake setup failed\r\n");
        goto low_power_abort;
    }
    bRtcStarted = 1U;
    app_low_power_set_stage(8U, "rtc-armed");

    /*
     * STOP前切到HSI。
     * 这样可以避免PLL在进入STOP前处于复杂状态，唤醒后再统一由SystemInit恢复144MHz。
     */
    if (!d_system_switch_to_hsi(APP_LOW_POWER_CLOCK_WAIT_TIMEOUT))
    {
        SEGGER_RTT_WriteString(0, "LP abort: HSI switch failed\r\n");
        goto low_power_abort;
    }
    app_low_power_set_stage(9U, "hsi-ready");

    /* IWDG不能关闭，低功耗前只拉大窗口并保持IWDGRSTEN有效。 */
    if (!d_watchdog_enter_low_power())
    {
        SEGGER_RTT_WriteString(0, "LP abort: watchdog low-power setup failed\r\n");
        goto low_power_abort;
    }
    bWatchdogLowPower = 1U;
    app_low_power_set_stage(10U, "watchdog-ready");

    /*
     * 冻结FreeRTOS节拍，避免STOP期间SysTick挂起导致WFI立即返回；唤醒后
     * 在时钟和外设恢复完毕后再恢复原配置，不把休眠时间计入RTOS业务时间。
     */
    u32SysTickCtrl = SysTick->CTRL & (SysTick_CTRL_ENABLE_Msk |
                                     SysTick_CTRL_TICKINT_Msk |
                                     SysTick_CTRL_CLKSOURCE_Msk);
    SysTick->CTRL = u32SysTickCtrl & ~(SysTick_CTRL_ENABLE_Msk | SysTick_CTRL_TICKINT_Msk);
    SysTick->VAL = 0UL;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
    bSysTickStopped = 1U;

low_power_sleep_cycle:
    bDidEnterStop = 0U;
    u8CanWkLevelBeforeWfi = 1U;
    u8CanWkExtiPendingBeforeWfi = 0U;
    /* 进入STOP前再次喂狗。 */
    d_watchdog_feed();
    s_app_low_power_sleep_flag = 1U;
    app_low_power_set_stage(11U, "wfi-armed");
    /* 最终检查与WFI之间短暂屏蔽中断，避免唤醒后仍错误进入休眠。 */
    u32PrimaskState = __get_PRIMASK();
    __disable_irq();
    u8CanWkLevelBeforeWfi = board_io_can_wk_level();
    u8CanWkExtiPendingBeforeWfi =
        (EXTI_GetStatusFlag(EXTI_LINE8) != RESET) ? 1U : 0U;
    /* 参考STOP例程的最后一道CAN_WK检查：既看软件标志，也看硬件pending和当前低电平。 */
    if ((s_app_low_power_can_wakeup != 0U) ||
        (u8CanWkExtiPendingBeforeWfi != 0U) ||
        (u8CanWkLevelBeforeWfi == 0U))
    {
        s_app_low_power_can_wakeup = 1U;
    }
    if ((s_app_low_power_afe_wakeup == 0U) &&
        (s_app_low_power_uart_wakeup == 0U) &&
        (s_app_low_power_can_wakeup == 0U) &&
        (s_app_low_power_key_wakeup == 0U) &&
        (s_app_low_power_detd_wakeup == 0U) &&
        (s_app_low_power_detc_wakeup == 0U) &&
        !d_rtc_wakeup_is_pending() &&
        (d_can_get_rx_activity_count() == s_app_low_power_last_can_rx_count) &&
        (d_can_get_tx_activity_count() == s_app_low_power_last_can_tx_count))
    {
        /* 中断保持pending时WFI会立即返回；恢复PRIMASK后由对应ISR置唤醒标志。 */
        __DSB();
        __ISB();
        d_system_enter_stop0();
        bDidEnterStop = 1U;
    }
    else
    {
        /* 最后一次检查发现已存在唤醒事件，不再执行WFI。 */
        s_app_low_power_stage = 12U;
    }
    s_app_low_power_sleep_flag = 0U;

    /* STOP返回后的第一件事先喂IWDG，仍在全局中断屏蔽状态内完成。 */
    if (bWatchdogLowPower != 0U)
    {
        d_watchdog_exit_low_power();
        bWatchdogLowPower = 0U;
    }
    __set_PRIMASK(u32PrimaskState);
    if (bDidEnterStop != 0U)
    {
        app_low_power_set_stage(13U, "wfi-returned");
    }
    else
    {
        app_low_power_set_stage(12U, "wfi-skipped");
    }

    /* 根据各中断标志记录本次唤醒原因，方便调试时在Watch窗口观察。 */
    if (s_app_low_power_afe_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_AFE;
    }
    else if (s_app_low_power_uart_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_UART;
    }
    else if (s_app_low_power_can_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_CAN;
    }
    else if (s_app_low_power_key_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_KEY;
    }
    else if (s_app_low_power_detd_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_DETD;
    }
    else if (s_app_low_power_detc_wakeup != 0U)
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_DETC;
    }
    else if (d_rtc_wakeup_is_pending())
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_RTC;
    }
    else
    {
        s_app_low_power_wake_reason = APP_LOW_POWER_WAKE_NONE;
    }

    if (s_app_low_power_wake_reason == APP_LOW_POWER_WAKE_RTC)
    {
        int32_t rtc_current_ma;
        uint8_t bCommTimeoutExpired = 0U;

        /* 暂停当前一次性RTC唤醒，之后低电流路径会重新装载20秒。 */
        if (bRtcStarted != 0U)
        {
            d_rtc_wakeup_stop();
            bRtcStarted = 0U;
        }

        app_low_power_set_stage(14U, "rtc-current-check");
#if (APP_PARAM_UDS_ENABLE == 1U)
        /* If the five-minute mute deadline expired in STOP, wake fully and restore communication. */
        bCommTimeoutExpired = (service_28_timeout_poll() == TRUE) ? 1U : 0U;
#endif
        if (app_low_power_read_rtc_current(&rtc_current_ma))
        {
            uint8_t bCurrentIdle = (rtc_current_ma == 0) ? 1U : 0U;
            uint8_t bOtherWake = ((app_low_power_has_non_rtc_wakeup() ||
                                   (bCommTimeoutExpired != 0U) ||
                                   (app_h_power_manage_remote_mos_timeout_due() != 0U))) ? 1U : 0U;

            if ((bCurrentIdle != 0U) && (bOtherWake == 0U))
            {
                /* 无明显电流且无其它唤醒：AFE重新Sleep、RTC重装20秒，直接回STOP。 */
                if (hsi_afe_lowpower(&gt_hsi_afe) &&
                    d_rtc_wakeup_start(APP_LOW_POWER_RTC_WAKE_SECONDS))
                {
                    bRtcStarted = 1U;
                    if (d_watchdog_enter_low_power())
                    {
                        bWatchdogLowPower = 1U;
                        app_low_power_set_stage(15U, "rtc-idle-resleep");
                        goto low_power_sleep_cycle;
                    }
                }

                SEGGER_RTT_WriteString(0, "LP RTC resleep setup failed; resume scheduler\r\n");
            }
        }
        else
        {
            SEGGER_RTT_WriteString(0, "LP RTC current read failed; resume scheduler for safety\r\n");
        }
    }

    /* 外部唤醒或电流超限：结束唤醒EXTI并恢复系统运行模块。 */
    if (bRtcStarted != 0U)
    {
        d_rtc_wakeup_stop();
        bRtcStarted = 0U;
    }
    app_low_power_close_wakeup_exti();
    app_low_power_restore_modules();
    if (bSysTickStopped != 0U)
    {
        SysTick->CTRL = 0UL;
        SysTick->VAL = 0UL;
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
        SysTick->CTRL = u32SysTickCtrl;
        bSysTickStopped = 0U;
        app_low_power_set_stage(26U, "systick-restored");
    }
    /* AFE Sleep保持MOS；恢复函数保留原命令，等待唤醒后的新样本再计算。 */
    (void)app_h_power_manage_restore_after_wakeup(&s_app_low_power_mos_output);

    /* 按CF_4872策略恢复调度器，休眠期间的FreeRTOS tick不追赶。 */
    if (bSchedulerSuspended != 0U)
    {
        (void)xTaskResumeAll();
        bSchedulerSuspended = 0U;
    }
    if (bAfeAcquired != 0U)
    {
        hsi_afe_release();
        bAfeAcquired = 0U;
    }

    /* RTC只是低功耗巡检；硬线/CAN/AFE/按键/串口唤醒后恢复客户周期报文。 */
    if ((s_app_low_power_wake_reason != APP_LOW_POWER_WAKE_RTC) &&
        (s_app_low_power_wake_reason != APP_LOW_POWER_WAKE_NONE))
    {
        app_can_notify_wakeup();
    }

    /* 完整恢复后重新累计普通空闲时间；RTC低电流分支会在上面的循环内直接继续STOP。 */
    s_app_low_power_idle_ms = 0U;
    app_low_power_set_stage(27U, "resume-complete");
    return;

low_power_abort:
    if (bWatchdogLowPower != 0U)
    {
        d_watchdog_exit_low_power();
    }
    if (bRtcStarted != 0U)
    {
        d_rtc_wakeup_stop();
    }
    if (bWakeExtiConfigured != 0U)
    {
        app_low_power_close_wakeup_exti();
    }
    if ((bAfeSleeping != 0U) ||
        (s_app_low_power_adc_suspended != 0U) ||
        (s_app_low_power_can_suspended != 0U) ||
        (s_app_low_power_timer_suspended != 0U) ||
        (s_app_low_power_uart_suspended != 0U))
    {
        app_low_power_restore_modules();
    }
    if (bSysTickStopped != 0U)
    {
        SysTick->CTRL = 0UL;
        SysTick->VAL = 0UL;
        SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk;
        SysTick->CTRL = u32SysTickCtrl;
    }
    if (bAfeSleeping != 0U)
    {
        (void)app_h_power_manage_restore_after_wakeup(&s_app_low_power_mos_output);
    }
    if (bSchedulerSuspended != 0U)
    {
        (void)xTaskResumeAll();
    }
    if (bAfeAcquired != 0U)
    {
        hsi_afe_release();
    }
    app_low_power_reset_idle_state();
}

void app_low_power_task(uint32_t elapsed_ms)
{
    app_bms_snapshot_t tAutoShutdownSnapshot;
    struct _app_bms_fault_t tFaultSnapshot;
    uint32_t u32TargetIdleMs;
    const uint32_t u32AutoShutdownTargetMs = APP_LOW_POWER_IDLE_MS;
    bool bIdleAllowed;
    bool bAutoShutdownAllowed;
    bool bOcdLocked;

    if (elapsed_ms == 0U)
    {
        elapsed_ms = 1U;
    }

    u32TargetIdleMs = app_low_power_target_idle_ms();
    bIdleAllowed = app_low_power_is_sleep_allowed();
    app_bms_fault_snapshot_read(&tFaultSnapshot);
    bOcdLocked = (tFaultSnapshot.tbms_sw_fault_flag.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U);
    app_bms_snapshot_read(&tAutoShutdownSnapshot);
    bAutoShutdownAllowed = (bool)(bIdleAllowed &&
                                  (tAutoShutdownSnapshot.data_ready != 0U) &&
                                  (tAutoShutdownSnapshot.signal.input.mcu_detc == 0U) &&
                                  (tAutoShutdownSnapshot.signal.input.mcu_detd == 0U));

    /* DETC/DETD均不在位且原有电流、通信等休眠门禁持续空闲时，累计自动关机时间。 */
    if (bAutoShutdownAllowed)
    {
        if ((u32AutoShutdownTargetMs - s_app_low_power_auto_shutdown_idle_ms) <= elapsed_ms)
        {
            s_app_low_power_auto_shutdown_idle_ms = u32AutoShutdownTargetMs;
        }
        else
        {
            s_app_low_power_auto_shutdown_idle_ms += elapsed_ms;
        }
    }
    else
    {
        s_app_low_power_auto_shutdown_idle_ms = 0U;
    }

    if (s_app_low_power_block_reason != APP_LOW_POWER_BLOCK_NONE)
    {
        /* bits0..6分别记录shutdown、IAP、CAN会话、唤醒、电流、外部输入、通信阻塞。 */
        s_app_low_power_block_diag_mask |=
            (uint32_t)(1UL << ((uint32_t)s_app_low_power_block_reason - 1UL));
    }
    if (bIdleAllowed)
    {
        if (bOcdLocked)
        {
            /* 过流锁期间不累计STOP空闲时间；自动关机计时仍可继续。 */
            s_app_low_power_idle_ms = 0U;
        }
        else if (u32TargetIdleMs == 0U)
        {
            /* 宗申无电流计时已完成；仍经过本轮完整低功耗门禁后立即STOP。 */
            s_app_low_power_idle_ms = 0U;
        }
        else if (s_app_low_power_idle_ms < u32TargetIdleMs)
        {
            /* 本次实时周期仍然满足低功耗条件，按实际毫秒数累计。 */
            if ((u32TargetIdleMs - s_app_low_power_idle_ms) <= elapsed_ms)
            {
                s_app_low_power_idle_ms = u32TargetIdleMs;
            }
            else
            {
                s_app_low_power_idle_ms += elapsed_ms;
            }
        }
    }
    else
    {
        /*
         * 任意条件不满足都重新计时。
         * 如果当前处于RTC快速回睡窗口，出现通信、电流、升级等有效运行条件后，
         * 立即退出快速回睡，后续恢复正常低功耗空闲判定。
         */
        app_low_power_reset_idle_state();
    }

    if ((APP_LOW_POWER_DIAG_PERIOD_MS - s_app_low_power_diag_elapsed_ms) <= elapsed_ms)
    {
        s_app_low_power_diag_elapsed_ms = APP_LOW_POWER_DIAG_PERIOD_MS;
    }
    else
    {
        s_app_low_power_diag_elapsed_ms += elapsed_ms;
    }

    if (s_app_low_power_diag_elapsed_ms >= APP_LOW_POWER_DIAG_PERIOD_MS)
    {
        s_app_low_power_diag_elapsed_ms = 0U;
        s_app_low_power_comm_check_valid = 0U;
        s_app_low_power_comm_diag_mask = 0U;
        s_app_low_power_block_diag_mask = 0U;
    }

    /* 过流锁只禁止STOP；DETC/DETD均不在位时仍可完成自动关机。 */
    if ((tAutoShutdownSnapshot.data_ready != 0U) &&
        (tAutoShutdownSnapshot.signal.input.mcu_detc == 0U) &&
        (tAutoShutdownSnapshot.signal.input.mcu_detd == 0U))
    {
        if (s_app_low_power_auto_shutdown_idle_ms >= u32AutoShutdownTargetMs)
        {
            SEGGER_RTT_printf(0,
                              "SHUTDOWN trigger=DETC_DETD_absent idle=%lums\r\n",
                              (unsigned long)s_app_low_power_auto_shutdown_idle_ms);
            s_app_low_power_auto_shutdown_idle_ms = 0U;
            (void)app_power_manage_request_shutdown();
        }
        return;
    }

    if (bIdleAllowed && !bOcdLocked &&
        ((u32TargetIdleMs == 0U) ||
         (s_app_low_power_idle_ms >= u32TargetIdleMs)))
    {
        /* 达到连续空闲时间后，执行完整STOP进入和唤醒恢复流程。 */
        app_low_power_enter_stop0();
    }
}

/* 复制低功耗内部计时和最近唤醒原因，供调试快照观察。 */

