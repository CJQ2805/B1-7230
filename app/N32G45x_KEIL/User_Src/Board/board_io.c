/*
 * 文件作用：
 *   板级IO映射和初始化模块，负责把原理图信号配置成当前APP需要的默认状态。
 *
 * 主要用途：
 *   1. 初始化当前 G45x 原理图定义的 MCU 引脚。
 *   2. 封装负载检测使能和检测电平读取。
 *   3. 维护当前原理图中信号名、端口、引脚和默认电平的对应关系。
 *
 * 适用范围：
 *   只适用于当前DC4803原理图版本。更换PCB或引脚定义时，应优先修改Board层，不扩散到业务模块。
 *
 * 调用关系：
 *   main.c 初始化时调用 board_io_init()；适配层、低功耗和少量过渡期业务模块调用板级动作接口。
 *
 * 注意事项：
 *   本文件只表达板级硬件事实，不写BMS保护策略；休眠状态下的IO配置必须兼顾功耗和外部电路安全。
 */
#include "Board/board_io.h"

/* 推挽输出初始化助手，用于板级默认安全状态。 */
static void board_io_init_output(d_gpio_port_t port, d_gpio_pin_t pin, uint8_t level)
{
    d_gpio_init(port, pin, d_GPIO_MODE_OUTPUT_PP, d_GPIO_NOPULL);
    d_gpio_set_state(port, pin, level);
}

/* 浮空输入初始化助手，需要偏置的地方由原理图外部电阻提供。 */
static void board_io_init_input(d_gpio_port_t port, d_gpio_pin_t pin)
{
    d_gpio_init(port, pin, d_GPIO_MODE_INPUT, d_GPIO_NOPULL);
}

/* 模拟输入初始化助手，用于ADC/NTC通道，避免数字输入漏电。 */
static void board_io_init_analog(d_gpio_port_t port, d_gpio_pin_t pin)
{
    d_gpio_init(port, pin, d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
}

void board_io_init(void)
{
    d_gpio_set_debug_mode(D_GPIO_DEBUG_SWD_ONLY);
    /* 保留SWD调试；当前原理图使用独立的PE口功能，不再占用JTAG复用脚。 */

    /* ADC通道：电压检测和NTC温度输入。 */
    board_io_init_analog(BOARD_IO_MCU_ADCDD_PORT, BOARD_IO_MCU_ADCDD_PIN);
    board_io_init_analog(BOARD_IO_MCU_NTC_PTC_PORT, BOARD_IO_MCU_NTC_PTC_PIN);
    board_io_init_analog(BOARD_IO_MCU_NTC_MOS_PORT, BOARD_IO_MCU_NTC_MOS_PIN);
    board_io_init_analog(BOARD_IO_MCU_NTC1_PORT, BOARD_IO_MCU_NTC1_PIN);
    board_io_init_analog(BOARD_IO_MCU_NTC2_PORT, BOARD_IO_MCU_NTC2_PIN);
    board_io_init_analog(BOARD_IO_MCU_NTC3_PORT, BOARD_IO_MCU_NTC3_PIN);

    /* 检测输入：保持无上下拉，跟随原理图外部电阻网络。 */
    board_io_init_input(BOARD_IO_MCU_DET_LOAD_PORT, BOARD_IO_MCU_DET_LOAD_PIN);
    board_io_init_input(BOARD_IO_MCU_2ND_DET_PORT, BOARD_IO_MCU_2ND_DET_PIN);
    board_io_init_input(BOARD_IO_MCU_DETD_PORT, BOARD_IO_MCU_DETD_PIN);
    board_io_init_input(BOARD_IO_MCU_DETC_PORT, BOARD_IO_MCU_DETC_PIN);
    board_io_init_input(BOARD_IO_MCU_SW_PORT, BOARD_IO_MCU_SW_PIN);

    /* 业务电源和MOS控制输出按当前原理图的上电策略初始化。 */

    board_io_init_output(BOARD_IO_MCU_DD_EN_PORT, BOARD_IO_MCU_DD_EN_PIN, 0U);
    board_io_init_output(BOARD_IO_MCU_PDSG_PORT, BOARD_IO_MCU_PDSG_PIN, 0U);
    board_io_init_output(BOARD_IO_MCU_LD_EN_PORT, BOARD_IO_MCU_LD_EN_PIN, 0U);
    board_io_init_output(BOARD_IO_MCU_FUSE_2ND_PORT, BOARD_IO_MCU_FUSE_2ND_PIN, 0U);
    /* V5V_EN默认打开。 */
    board_io_init_output(BOARD_IO_V5V_EN_PORT, BOARD_IO_V5V_EN_PIN, 1U);
    /* LD1~LD5低电平点亮，初始化为高电平确保默认熄灭。 */
    board_io_init_output(BOARD_IO_MCU_LD1_PORT, BOARD_IO_MCU_LD1_PIN, 1U);
    board_io_init_output(BOARD_IO_MCU_LD2_PORT, BOARD_IO_MCU_LD2_PIN, 1U);
    board_io_init_output(BOARD_IO_MCU_LD3_PORT, BOARD_IO_MCU_LD3_PIN, 1U);
    board_io_init_output(BOARD_IO_MCU_LD4_PORT, BOARD_IO_MCU_LD4_PIN, 1U);
    board_io_init_output(BOARD_IO_MCU_LD5_PORT, BOARD_IO_MCU_LD5_PIN, 1U);
    board_io_init_output(BOARD_IO_FLASH_SPI_CS_PORT, BOARD_IO_FLASH_SPI_CS_PIN, 1U);

    /* NTC采样使能默认打开，保证上电后温度采样通道可直接工作。 */
    board_io_init_output(BOARD_IO_MCU_NTC_EN_PORT, BOARD_IO_MCU_NTC_EN_PIN, 1U);

    /* DVC1124的COFF/DOFF为低有效硬关断脚，空闲拉高表示允许AFE输出。 */
    board_io_init_output(BOARD_IO_AFE_COFF_PORT, BOARD_IO_AFE_COFF_PIN, 1U);
    board_io_init_output(BOARD_IO_AFE_DOFF_PORT, BOARD_IO_AFE_DOFF_PIN, 1U);
    board_io_init_output(BOARD_IO_DCDC_EN_PORT, BOARD_IO_DCDC_EN_PIN, 1U);
    board_io_init_output(BOARD_IO_MOS_DR_EN_PORT, BOARD_IO_MOS_DR_EN_PIN, 1U);	
    /* CAN隔离侧保持上电，CAN_WK才能在低功耗期间产生唤醒边沿。 */
    board_io_init_output(BOARD_IO_CAN_POWER_EN_PORT, BOARD_IO_CAN_POWER_EN_PIN, 1U);
    board_io_init_input(BOARD_IO_CAN_WK_PORT, BOARD_IO_CAN_WK_PIN);
    /* LED默认关闭，运行指示需要单独由业务模块打开，避免低功耗前遗留点亮状态。 */
    board_io_init_output(BOARD_IO_LED_PORT, BOARD_IO_LED_PIN, BOARD_IO_LED_OFF_LEVEL);
    /* PA3已配置为MCU_NTC_MOS，任务心跳脚按空IO处理，避免破坏ADC配置。 */
}

void board_io_led_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_LED_PORT, BOARD_IO_LED_PIN, level);
}

void board_io_led_toggle(void)
{
    uint8_t level;

    level = d_gpio_get_state(BOARD_IO_LED_PORT, BOARD_IO_LED_PIN);
    if (level == BOARD_IO_LED_OFF_LEVEL)
    {
        board_io_led_set(BOARD_IO_LED_ON_LEVEL);
    }
    else
    {
        board_io_led_set(BOARD_IO_LED_OFF_LEVEL);
    }
}

void board_io_mcu_ntc_enable(uint8_t enable)
{
    d_gpio_set_state(BOARD_IO_MCU_NTC_EN_PORT, BOARD_IO_MCU_NTC_EN_PIN, enable ? 1U : 0U);
}

void board_io_dcdc_enable(uint8_t enable)
{
    d_gpio_set_state(BOARD_IO_DCDC_EN_PORT, BOARD_IO_DCDC_EN_PIN, enable ? 1U : 0U);
}

void board_io_can_power_enable(uint8_t enable)
{
    d_gpio_set_state(BOARD_IO_CAN_POWER_EN_PORT, BOARD_IO_CAN_POWER_EN_PIN,
                     enable ? 1U : 0U);
}

void board_io_load_detect_enable(uint8_t enable_level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD_EN_PORT, BOARD_IO_MCU_LD_EN_PIN, enable_level);
}

uint8_t board_io_load_detect_enabled(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_LD_EN_PORT, BOARD_IO_MCU_LD_EN_PIN);
}

uint8_t board_io_load_detect_level(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_DET_LOAD_PORT, BOARD_IO_MCU_DET_LOAD_PIN);
}

uint8_t board_io_mcu_2nd_det_level(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_2ND_DET_PORT, BOARD_IO_MCU_2ND_DET_PIN);
}

uint8_t board_io_mcu_detd_level(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_DETD_PORT, BOARD_IO_MCU_DETD_PIN);
}

uint8_t board_io_mcu_detc_level(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_DETC_PORT, BOARD_IO_MCU_DETC_PIN);
}

uint8_t board_io_mcu_sw_level(void)
{
    return d_gpio_get_state(BOARD_IO_MCU_SW_PORT, BOARD_IO_MCU_SW_PIN);
}

uint8_t board_io_afe_alert_level(void)
{
    return d_gpio_get_state(BOARD_IO_AFE_ALERT_PORT, BOARD_IO_AFE_ALERT_PIN);
}

uint8_t board_io_can_wk_level(void)
{
    return d_gpio_get_state(BOARD_IO_CAN_WK_PORT, BOARD_IO_CAN_WK_PIN);
}

void board_io_v5v_enable(uint8_t enable)
{
    d_gpio_set_state(BOARD_IO_V5V_EN_PORT, BOARD_IO_V5V_EN_PIN, enable ? 1U : 0U);
}

void board_io_mcu_ld1_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD1_PORT, BOARD_IO_MCU_LD1_PIN, level ? 1U : 0U);
}

void board_io_mcu_ld2_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD2_PORT, BOARD_IO_MCU_LD2_PIN, level ? 1U : 0U);
}

void board_io_mcu_ld3_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD3_PORT, BOARD_IO_MCU_LD3_PIN, level ? 1U : 0U);
}

void board_io_mcu_ld4_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD4_PORT, BOARD_IO_MCU_LD4_PIN, level ? 1U : 0U);
}

void board_io_mcu_ld5_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_MCU_LD5_PORT, BOARD_IO_MCU_LD5_PIN, level ? 1U : 0U);
}

void board_io_flash_spi_cs_set(uint8_t level)
{
    d_gpio_set_state(BOARD_IO_FLASH_SPI_CS_PORT, BOARD_IO_FLASH_SPI_CS_PIN,
                     level ? 1U : 0U);
}

