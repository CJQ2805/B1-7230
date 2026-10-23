#ifndef _BOARD_IO_H_
#define _BOARD_IO_H_

#include "Driver/d_gpio.h"

/* 当前原理图中仍由APP使用的MCU ADC输入引脚。 */
#define BOARD_IO_MCU_ADCDD_PORT       D_GPIO_PORT_A
#define BOARD_IO_MCU_ADCDD_PIN        D_GPIO_PIN(1U)
#define BOARD_IO_MCU_NTC_PTC_PORT     D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC_PTC_PIN      D_GPIO_PIN(2U)
#define BOARD_IO_MCU_NTC_MOS_PORT     D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC_MOS_PIN      D_GPIO_PIN(3U)
#define BOARD_IO_MCU_NTC1_PORT        D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC1_PIN         D_GPIO_PIN(4U)
#define BOARD_IO_MCU_NTC2_PORT        D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC2_PIN         D_GPIO_PIN(5U)
#define BOARD_IO_MCU_NTC3_PORT        D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC3_PIN         D_GPIO_PIN(6U)

/* 当前原理图中仍由APP使用的电源、MOS和NTC控制引脚。 */
#define BOARD_IO_DCDC_EN_PORT         D_GPIO_PORT_C
#define BOARD_IO_DCDC_EN_PIN          D_GPIO_PIN(4U)
#define BOARD_IO_MCU_DD_EN_PORT       D_GPIO_PORT_A
#define BOARD_IO_MCU_DD_EN_PIN        D_GPIO_PIN(0U)
#define BOARD_IO_MCU_PDSG_PORT        D_GPIO_PORT_C
#define BOARD_IO_MCU_PDSG_PIN         D_GPIO_PIN(5U)
#define BOARD_IO_MCU_2ND_DET_PORT     D_GPIO_PORT_B
#define BOARD_IO_MCU_2ND_DET_PIN      D_GPIO_PIN(0U)
#define BOARD_IO_MCU_LD_EN_PORT       D_GPIO_PORT_E
#define BOARD_IO_MCU_LD_EN_PIN        D_GPIO_PIN(8U)

#define BOARD_IO_MOS_DR_EN_PORT       D_GPIO_PORT_E
#define BOARD_IO_MOS_DR_EN_PIN        D_GPIO_PIN(7U)
#define BOARD_IO_MCU_DET_LOAD_PORT    D_GPIO_PORT_E
#define BOARD_IO_MCU_DET_LOAD_PIN     D_GPIO_PIN(9U)
#define BOARD_IO_MCU_NTC_EN_PORT      D_GPIO_PORT_A
#define BOARD_IO_MCU_NTC_EN_PIN       D_GPIO_PIN(7U)

/* 新增原理图输入信号：DETD、DETC和外部开关状态。 */
#define BOARD_IO_MCU_DETD_PORT        D_GPIO_PORT_C
#define BOARD_IO_MCU_DETD_PIN         D_GPIO_PIN(6U)
#define BOARD_IO_MCU_DETC_PORT        D_GPIO_PORT_D
#define BOARD_IO_MCU_DETC_PIN         D_GPIO_PIN(15U)
#define BOARD_IO_MCU_SW_PORT          D_GPIO_PORT_D
#define BOARD_IO_MCU_SW_PIN           D_GPIO_PIN(1U)

/* 新增原理图输出信号：5V使能和负载控制LD1~LD5。 */
#define BOARD_IO_V5V_EN_PORT          D_GPIO_PORT_D
#define BOARD_IO_V5V_EN_PIN           D_GPIO_PIN(13U)
#define BOARD_IO_MCU_LD1_PORT         D_GPIO_PORT_D
#define BOARD_IO_MCU_LD1_PIN          D_GPIO_PIN(2U)
#define BOARD_IO_MCU_LD2_PORT         D_GPIO_PORT_D
#define BOARD_IO_MCU_LD2_PIN          D_GPIO_PIN(3U)
#define BOARD_IO_MCU_LD3_PORT         D_GPIO_PORT_D
#define BOARD_IO_MCU_LD3_PIN          D_GPIO_PIN(4U)
#define BOARD_IO_MCU_LD4_PORT         D_GPIO_PORT_D
#define BOARD_IO_MCU_LD4_PIN          D_GPIO_PIN(5U)
#define BOARD_IO_MCU_LD5_PORT         D_GPIO_PORT_D
#define BOARD_IO_MCU_LD5_PIN          D_GPIO_PIN(6U)

/* 外部FLASH使用SPI2默认引脚：PB12=CS、PB13=CLK、PB14=MISO、PB15=MOSI。 */
#define BOARD_IO_FLASH_SPI_CS_PORT    D_GPIO_PORT_B
#define BOARD_IO_FLASH_SPI_CS_PIN     D_GPIO_PIN(12U)
#define BOARD_IO_FLASH_SPI_CLK_PORT   D_GPIO_PORT_B
#define BOARD_IO_FLASH_SPI_CLK_PIN    D_GPIO_PIN(13U)
#define BOARD_IO_FLASH_SPI_MISO_PORT  D_GPIO_PORT_B
#define BOARD_IO_FLASH_SPI_MISO_PIN   D_GPIO_PIN(14U)
#define BOARD_IO_FLASH_SPI_MOSI_PORT  D_GPIO_PORT_B
#define BOARD_IO_FLASH_SPI_MOSI_PIN   D_GPIO_PIN(15U)

/* CAN1使用当前原理图默认引脚：PA11=RX、PA12=TX；PC8唤醒，PC9隔离侧5V使能。 */
#define BOARD_IO_CAN_RX_PORT          D_GPIO_PORT_A
#define BOARD_IO_CAN_RX_PIN           D_GPIO_PIN(11U)
#define BOARD_IO_CAN_TX_PORT          D_GPIO_PORT_A
#define BOARD_IO_CAN_TX_PIN           D_GPIO_PIN(12U)
#define BOARD_IO_CAN_WK_PORT          D_GPIO_PORT_C
#define BOARD_IO_CAN_WK_PIN           D_GPIO_PIN(8U)
#define BOARD_IO_CAN_POWER_EN_PORT    D_GPIO_PORT_C
#define BOARD_IO_CAN_POWER_EN_PIN     D_GPIO_PIN(9U)

/* 新版原理图调试串口：释放JTAG后将USART2重映射到PB4/PB5。 */
#define BOARD_IO_MCU_TX_PORT          D_GPIO_PORT_B
#define BOARD_IO_MCU_TX_PIN           D_GPIO_PIN(4U)
#define BOARD_IO_MCU_RX_PORT          D_GPIO_PORT_B
#define BOARD_IO_MCU_RX_PIN           D_GPIO_PIN(5U)


/* 当前原理图中的二级熔断、LED和DVC1124 AFE相关引脚。 */
#define BOARD_IO_MCU_FUSE_2ND_PORT    D_GPIO_PORT_B
#define BOARD_IO_MCU_FUSE_2ND_PIN     D_GPIO_PIN(1U)
#define BOARD_IO_LED_PORT             D_GPIO_PORT_C
#define BOARD_IO_LED_PIN              D_GPIO_PIN(13U)
#define BOARD_IO_LED_OFF_LEVEL        1U
#define BOARD_IO_LED_ON_LEVEL         0U
#define BOARD_IO_AFE_SCL_PORT         D_GPIO_PORT_B
#define BOARD_IO_AFE_SCL_PIN          D_GPIO_PIN(10U)
#define BOARD_IO_AFE_SDA_PORT         D_GPIO_PORT_B
#define BOARD_IO_AFE_SDA_PIN          D_GPIO_PIN(11U)
#define BOARD_IO_AFE_COFF_PORT        D_GPIO_PORT_E
#define BOARD_IO_AFE_COFF_PIN         D_GPIO_PIN(13U)
#define BOARD_IO_AFE_DOFF_PORT        D_GPIO_PORT_E
#define BOARD_IO_AFE_DOFF_PIN         D_GPIO_PIN(12U)
#define BOARD_IO_AFE_ALERT_PORT       D_GPIO_PORT_E
#define BOARD_IO_AFE_ALERT_PIN        D_GPIO_PIN(11U)

/* 当前原理图没有DVC1124的5V使能脚，vdd5保持为空IO。 */
#define BOARD_IO_AFE_DVC1124_IOS_INIT                                      \
    {                                                                       \
        .vdd5 = {D_GPIO_PORT_INVALID, 0U},                                  \
        .chg_ctrl = {BOARD_IO_AFE_COFF_PORT, BOARD_IO_AFE_COFF_PIN},        \
        .dsg_ctl = {BOARD_IO_AFE_DOFF_PORT, BOARD_IO_AFE_DOFF_PIN},         \
        .alert = {BOARD_IO_AFE_ALERT_PORT, BOARD_IO_AFE_ALERT_PIN}          \
    }

/* 按当前 G45x 原理图初始化用户 IO。 */
void board_io_init(void);
/* 设置运行指示LED电平，level使用BOARD_IO_LED_ON_LEVEL或BOARD_IO_LED_OFF_LEVEL。 */
void board_io_led_set(uint8_t level);
/* 翻转运行指示LED，用于APP主循环低频心跳。 */
void board_io_led_toggle(void);
/* 控制MCU侧NTC采样使能脚，enable为1时打开NTC采样上拉电源。 */
void board_io_mcu_ntc_enable(uint8_t enable);
/* 控制DCDC_EN电源使能，enable为1打开，0关闭。 */
void board_io_dcdc_enable(uint8_t enable);
/* 控制隔离CAN侧5V电源；低功耗前关闭，唤醒后恢复。 */
void board_io_can_power_enable(uint8_t enable);
/* 控制负载检测电路使能脚，level由上层配置决定。 */
void board_io_load_detect_enable(uint8_t enable_level);
/* 读取PE8实际电平：1=负载检测电路已使能，0=未使能。 */
uint8_t board_io_load_detect_enabled(void);
/* 读取负载检测输入脚，返回当前MCU_DET_LOAD原始电平。 */
uint8_t board_io_load_detect_level(void);
/* 读取二级检测输入。 */
uint8_t board_io_mcu_2nd_det_level(void);
/* 读取新增DETD输入信号。 */
uint8_t board_io_mcu_detd_level(void);
/* 读取新增DETC输入信号。 */
uint8_t board_io_mcu_detc_level(void);
/* 读取新增MCU_SW输入信号。 */
uint8_t board_io_mcu_sw_level(void);
/* 读取AFE报警输入。 */
uint8_t board_io_afe_alert_level(void);
/* 读取CAN唤醒输入。 */
uint8_t board_io_can_wk_level(void);
/* 控制新增V5V_EN输出信号。 */
void board_io_v5v_enable(uint8_t enable);
/* 控制新增MCU_LD1~MCU_LD5输出信号。 */
void board_io_mcu_ld1_set(uint8_t level);
void board_io_mcu_ld2_set(uint8_t level);
void board_io_mcu_ld3_set(uint8_t level);
void board_io_mcu_ld4_set(uint8_t level);
void board_io_mcu_ld5_set(uint8_t level);
/* 控制外部FLASH片选，低电平选中。 */
void board_io_flash_spi_cs_set(uint8_t level);
/* 翻转BMS任务运行测试脚，不改变业务IO。 */
void board_io_task_heartbeat_toggle(void);

#endif
