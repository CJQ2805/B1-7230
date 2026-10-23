#ifndef __DUCAR_UART_PROTOCOL_H__
#define __DUCAR_UART_PROTOCOL_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * 模块边界：BMS帧解析/组帧、全部查询与写控制命令、UART应答以及LOG导出。
 * APP迁移BMS协议时以本模块的.c/.h为完整入口，不再依赖App目录下的协议管理文件。
 */

/* DUCAR智能BMS普通通信协议固定字段。 */
#define DUCAR_BMS_SOF                 ((uint8_t)0xAAU)
#define DUCAR_BMS_ADDR                ((uint8_t)0x01U)
#define DUCAR_BMS_EOF0                ((uint8_t)0x3AU)
#define DUCAR_BMS_EOF1                ((uint8_t)0x4BU)

/* 当前协议只使用1字节LEN；LOG导出单帧DATA为73字节，所以这里预留到96字节。 */
#define DUCAR_BMS_MAX_DATA_LEN        ((uint8_t)96U)
#define DUCAR_BMS_MAX_FRAME_LEN       ((uint16_t)(1U + 1U + 1U + 1U + DUCAR_BMS_MAX_DATA_LEN + 1U + 2U))

/* 通用ACK/NAK，文档未单独定义错误帧时用同一命令回1字节状态。 */
#define DUCAR_BMS_ACK                 ((uint8_t)0x00U)
#define DUCAR_BMS_NAK                 ((uint8_t)0x01U)

/* DUCAR智能BMS普通通信命令字。 */
#define DUCAR_BMS_CMD_GET_TEMPERATURE           ((uint8_t)0x05U)
#define DUCAR_BMS_CMD_GET_MOS_TEMPERATURE       ((uint8_t)0x06U)
#define DUCAR_BMS_CMD_GET_MCU_TEMPERATURE       ((uint8_t)0x07U)
#define DUCAR_BMS_CMD_GET_BATTERY_VOLTAGE       ((uint8_t)0x08U)
#define DUCAR_BMS_CMD_GET_CURRENT               ((uint8_t)0x09U)
#define DUCAR_BMS_CMD_GET_STATUS                ((uint8_t)0x0AU)
#define DUCAR_BMS_CMD_GET_CELL_VOLT_EXTREME     ((uint8_t)0x0BU)
#define DUCAR_BMS_CMD_GET_CELLV_CMD1            ((uint8_t)0x0CU)
#define DUCAR_BMS_CMD_GET_CELLV_CMD2            ((uint8_t)0x0DU)
#define DUCAR_BMS_CMD_GET_CELLV_CMD3            ((uint8_t)0x0EU)
#define DUCAR_BMS_CMD_GET_SOC                   ((uint8_t)0x0FU)
#define DUCAR_BMS_CMD_GET_SOH                   ((uint8_t)0x10U)
#define DUCAR_BMS_CMD_GET_REMAIN_CAPACITY       ((uint8_t)0x11U)
#define DUCAR_BMS_CMD_GET_FULL_CHARGE_CAPACITY  ((uint8_t)0x12U)
#define DUCAR_BMS_CMD_GET_CYCLE_COUNT           ((uint8_t)0x13U)
#define DUCAR_BMS_CMD_GET_DESIGN_CAPACITY       ((uint8_t)0x14U)
#define DUCAR_BMS_CMD_GET_DESIGN_VOLTAGE        ((uint8_t)0x15U)
#define DUCAR_BMS_CMD_GET_MANUFACTURE_DATE      ((uint8_t)0x16U)
#define DUCAR_BMS_CMD_GET_SERIAL_NUMBER         ((uint8_t)0x17U)
#define DUCAR_BMS_CMD_GET_DEVICE_NAME           ((uint8_t)0x18U)
#define DUCAR_BMS_CMD_GET_SOFTWARE_VERSION      ((uint8_t)0x19U)
#define DUCAR_BMS_CMD_GET_HARDWARE_VERSION      ((uint8_t)0x1AU)
#define DUCAR_BMS_CMD_GET_CUSTOMER_SW_VERSION   ((uint8_t)0x1BU)
#define DUCAR_BMS_CMD_GET_CUSTOMER_HW_VERSION   ((uint8_t)0x1CU)
#define DUCAR_BMS_CMD_GET_BATTERY_BALANCE_STAT  ((uint8_t)0x1DU)
#define DUCAR_BMS_CMD_GET_CELL_VOLTAGE_EXTREMA_NUM ((uint8_t)0x1EU)
#define DUCAR_BMS_CMD_LOG_RECORD                ((uint8_t)0x49U)
#define DUCAR_BMS_CMD_GET_FIRST_USED_TIME       DUCAR_BMS_CMD_LOG_RECORD
#define DUCAR_BMS_CMD_SET_SERIAL_NUMBER         ((uint8_t)0x62U)
#define DUCAR_BMS_CMD_SET_RTC                   ((uint8_t)0x63U)
#define DUCAR_BMS_CMD_GET_RTC                   ((uint8_t)0x64U)
#define DUCAR_BMS_CMD_SET_BMS_SHUTDOWN          ((uint8_t)0x66U)
#define DUCAR_BMS_CMD_SET_BMS_RESET             ((uint8_t)0x67U)
#define DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR       ((uint8_t)0x68U)
#define DUCAR_BMS_CMD_SOC_FORCE_OCV              ((uint8_t)0x69U)
#define DUCAR_BMS_CMD_SET_SOC                    ((uint8_t)0x70U)
#define DUCAR_BMS_CMD_SET_CURRENT_CALIBRATION    ((uint8_t)0x71U) /* 写入一个实际电流校准点。 */
#define DUCAR_BMS_CMD_GET_CURRENT_CALIBRATION    ((uint8_t)0x72U) /* 读取K_PPM和B。 */

typedef enum
{
    DUCAR_BMS_PARSE_NONE = 0,   /* 当前字节还没有组成完整帧。 */
    DUCAR_BMS_PARSE_OK,         /* 收到一帧完整且校验正确的数据。 */
    DUCAR_BMS_PARSE_ERROR       /* 收到异常帧，解析器已经复位。 */
} ducar_bms_parse_result_t;

typedef struct
{
    uint8_t cmd;                         /* 命令字。 */
    uint8_t len;                         /* DATA长度。 */
    uint8_t data[DUCAR_BMS_MAX_DATA_LEN];/* DATA内容。 */
} ducar_bms_frame_t;

typedef struct
{
    uint8_t state;                       /* 当前解析状态。 */
    uint8_t data_index;                  /* 已经接收的DATA字节数。 */
    uint8_t checksum;                    /* 从SOF开始的8位累加校验和。 */
    ducar_bms_frame_t frame;             /* 正在解析的帧。 */
} ducar_bms_parser_t;

/* 复用UART 0x49字段映射，编码CAN F232的60字节载荷：索引2、长度1、日志体57。 */
bool ducar_bms_pack_log_payload(uint16_t index, uint8_t out[60]);

void ducar_bms_parser_init(ducar_bms_parser_t *parser);
ducar_bms_parse_result_t ducar_bms_parser_input(ducar_bms_parser_t *parser, uint8_t byte, ducar_bms_frame_t *out_frame);
uint16_t ducar_bms_pack_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out_frame, uint16_t out_size);

void ducar_uart_protocol_init(void);
void ducar_bms_handle_frame(const ducar_bms_frame_t *frame);
bool ducar_bms_send_status(uint8_t cmd, uint8_t status);
void ducar_bms_uart_log_task(void);

#endif
