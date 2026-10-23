/*
 * 文件作用：
 *   Ducar BMS完整协议模块，负责BMS帧格式、命令语义、业务回复和UART适配。
 *
 * 主要用途：
 *   1. 解析和生成AA 01 CMD LEN DATA SUM 3A 4B协议帧。
 *   2. 处理BMS全部查询、写入和控制命令。
 *   3. 通过当前G45x UART发送BMS响应，并维护LOG异步导出。
 */
#include "Protocol/Comm/ducar_uart_protocol.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"

#include "App/app_bms_types.h"
#include "App/app_balance_manage.h"
#include "App/app_bms_snapshot.h"
#include "App/app_current_calib.h"
#include "App/app_hw_apply.h"
#include "App/app_log_manage.h"
#include "App/app_power_manage.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "App/app_bms_fault_config.h"
#include "App/app_bms_fault.h"
#include "Driver/d_system.h"
#include "Driver/d_timer.h"
#include "Driver/d_uart.h"
#include "Driver/d_rtc.h"
#include "Task/bms_task.h"
#include "app_soc.h"
#include <string.h>

typedef enum
{
    DUCAR_BMS_STATE_WAIT_SOF = 0U, /* 等待帧头0xAA。 */
    DUCAR_BMS_STATE_ADDR,          /* 接收地址0x01。 */
    DUCAR_BMS_STATE_CMD,           /* 接收命令字。 */
    DUCAR_BMS_STATE_LEN,           /* 接收DATA长度。 */
    DUCAR_BMS_STATE_DATA,          /* 接收DATA内容。 */
    DUCAR_BMS_STATE_SUM,           /* 接收8位累加校验和。 */
    DUCAR_BMS_STATE_EOF0,          /* 接收帧尾0x3A。 */
    DUCAR_BMS_STATE_EOF1           /* 接收帧尾0x4B。 */
} ducar_bms_state_t;

/* 复位BMS帧解析器，清空当前帧状态并等待新的帧头。 */
static void ducar_bms_parser_reset(ducar_bms_parser_t *parser)
{
    parser->state = (uint8_t)DUCAR_BMS_STATE_WAIT_SOF;
    parser->data_index = 0U;
    parser->checksum = 0U;
    parser->frame.cmd = 0U;
    parser->frame.len = 0U;
}

/* 处理BMS帧解析错误，保留命令字并复位解析器。 */
static ducar_bms_parse_result_t ducar_bms_parser_error(ducar_bms_parser_t *parser, ducar_bms_frame_t *out_frame)
{
    if (out_frame != 0)
    {
        *out_frame = parser->frame; /* 保留命令字，便于上层需要时按原命令回复NAK。 */
    }

    ducar_bms_parser_reset(parser);
    return DUCAR_BMS_PARSE_ERROR;
}

/* 初始化BMS协议解析器。 */
void ducar_bms_parser_init(ducar_bms_parser_t *parser)
{
    if (parser == 0)
    {
        return;
    }

    ducar_bms_parser_reset(parser);
}

/* 输入一个字节并推进BMS协议帧解析状态机。 */
ducar_bms_parse_result_t ducar_bms_parser_input(ducar_bms_parser_t *parser, uint8_t byte, ducar_bms_frame_t *out_frame)
{
    if (parser == 0)
    {
        return DUCAR_BMS_PARSE_NONE;
    }

    switch ((ducar_bms_state_t)parser->state)
    {
        case DUCAR_BMS_STATE_WAIT_SOF:
            if (byte == DUCAR_BMS_SOF)
            {
                ducar_bms_parser_reset(parser);
                parser->checksum = DUCAR_BMS_SOF;
                parser->state = (uint8_t)DUCAR_BMS_STATE_ADDR;
            }
            break;

        case DUCAR_BMS_STATE_ADDR:
            if (byte != DUCAR_BMS_ADDR)
            {
                ducar_bms_parser_reset(parser);
                if (byte == DUCAR_BMS_SOF)
                {
                    parser->checksum = DUCAR_BMS_SOF;
                    parser->state = (uint8_t)DUCAR_BMS_STATE_ADDR;
                    break;
                }
                return DUCAR_BMS_PARSE_ERROR;
            }

            parser->checksum = (uint8_t)(parser->checksum + byte);
            parser->state = (uint8_t)DUCAR_BMS_STATE_CMD;
            break;

        case DUCAR_BMS_STATE_CMD:
            parser->frame.cmd = byte;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            parser->state = (uint8_t)DUCAR_BMS_STATE_LEN;
            break;

        case DUCAR_BMS_STATE_LEN:
            if (byte > DUCAR_BMS_MAX_DATA_LEN)
            {
                return ducar_bms_parser_error(parser, out_frame);
            }

            parser->frame.len = byte;
            parser->data_index = 0U;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->frame.len == 0U)
            {
                parser->state = (uint8_t)DUCAR_BMS_STATE_SUM;
            }
            else
            {
                parser->state = (uint8_t)DUCAR_BMS_STATE_DATA;
            }
            break;

        case DUCAR_BMS_STATE_DATA:
            parser->frame.data[parser->data_index] = byte;
            parser->data_index++;
            parser->checksum = (uint8_t)(parser->checksum + byte);
            if (parser->data_index >= parser->frame.len)
            {
                parser->state = (uint8_t)DUCAR_BMS_STATE_SUM;
            }
            break;

        case DUCAR_BMS_STATE_SUM:
            if (byte != parser->checksum)
            {
                return ducar_bms_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_BMS_STATE_EOF0;
            break;

        case DUCAR_BMS_STATE_EOF0:
            if (byte != DUCAR_BMS_EOF0)
            {
                return ducar_bms_parser_error(parser, out_frame);
            }

            parser->state = (uint8_t)DUCAR_BMS_STATE_EOF1;
            break;

        case DUCAR_BMS_STATE_EOF1:
            if (byte != DUCAR_BMS_EOF1)
            {
                return ducar_bms_parser_error(parser, out_frame);
            }

            if (out_frame != 0)
            {
                *out_frame = parser->frame;
            }
            ducar_bms_parser_reset(parser);
            return DUCAR_BMS_PARSE_OK;

        default:
            ducar_bms_parser_reset(parser);
            break;
    }

    return DUCAR_BMS_PARSE_NONE;
}

/* 按照BMS协议格式组装一帧待发送数据。 */
uint16_t ducar_bms_pack_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out_frame, uint16_t out_size)
{
    uint8_t u8Checksum;
    uint16_t u16FrameIndex;
    uint8_t u8DataIndex;

    if ((out_frame == 0) || (out_size < (uint16_t)(7U + len)) || (len > DUCAR_BMS_MAX_DATA_LEN) ||
        ((data == 0) && (len > 0U)))
    {
        return 0U;
    }

    u16FrameIndex = 0U;
    u8Checksum = (uint8_t)(DUCAR_BMS_SOF + DUCAR_BMS_ADDR);

    out_frame[u16FrameIndex++] = DUCAR_BMS_SOF;
    out_frame[u16FrameIndex++] = DUCAR_BMS_ADDR;

    out_frame[u16FrameIndex++] = cmd;
    u8Checksum = (uint8_t)(u8Checksum + cmd);

    out_frame[u16FrameIndex++] = len;
    u8Checksum = (uint8_t)(u8Checksum + len);

    for (u8DataIndex = 0U; u8DataIndex < len; u8DataIndex++)
    {
        out_frame[u16FrameIndex++] = data[u8DataIndex];
        u8Checksum = (uint8_t)(u8Checksum + data[u8DataIndex]);
    }

    out_frame[u16FrameIndex++] = u8Checksum;
    out_frame[u16FrameIndex++] = DUCAR_BMS_EOF0;
    out_frame[u16FrameIndex++] = DUCAR_BMS_EOF1;

    return u16FrameIndex;
}


/* 协议版本先和文档版本保持一致，后续有正式软件版本管理时只改这里。 */
/* 当前项目按配置串数估算磷酸铁锂默认设计电压，后续有参数区后改为参数读取。 */
#define DUCAR_BMS_DESIGN_CELL_MV           ((uint32_t)3200UL)
#define DUCAR_BMS_TEMP_INVALID_0P1K         ((uint16_t)0xFFFFU) /* 温度通道无效时按协议返回0xFFFF。 */

#define DUCAR_BMS_LOG_COUNT_REQ            ((uint8_t)0x20U) /* 上位机读取故障记录总条数的请求码。 */
#define DUCAR_BMS_LOG_EXPORT_IDLE          ((uint8_t)0U)    /* 当前没有LOG导出任务。 */
#define DUCAR_BMS_LOG_EXPORT_BUSY          ((uint8_t)1U)    /* 当前正在逐帧导出LOG。 */
#define DUCAR_BMS_LOG_EXPORT_INTERVAL_MS   ((uint32_t)1000UL) /* LOG导出间隔，避免连续发送影响系统运行。 */
#define DUCAR_BMS_SOC_CMD_NONE             ((uint8_t)0U)
#define DUCAR_BMS_SOC_OCV_TIMEOUT_MS       ((uint32_t)5000UL)

/* 0x49协议主体从时间到协议校验字段共57字节，包含独立软件告警字段。 */
#define DUCAR_BMS_LOG_PROTOCOL_BODY_SIZE   ((uint8_t)57U)
/* 0x49 DATA由索引、主体长度和协议主体组成。 */
#define DUCAR_BMS_LOG_PROTOCOL_DATA_LEN    ((uint8_t)(2U + 1U + DUCAR_BMS_LOG_PROTOCOL_BODY_SIZE))

typedef struct
{
    uint8_t state;              /* 当前导出状态，0空闲，1正在导出。 */
    uint16_t request_count;     /* 上位机本次请求导出的总条数。 */
    uint16_t next_index;        /* 下一条准备发送的LOG索引，1起始。 */
    uint32_t last_send_ms;      /* 上一帧LOG发送时间，用于限速。 */
} ducar_bms_log_export_t;

static uint8_t s_ducar_bms_tx_frame[DUCAR_BMS_MAX_FRAME_LEN];              /* 普通BMS协议发送帧缓存。 */
static ducar_bms_log_export_t s_ducar_bms_log_export;                        /* LOG流式导出状态。 */
static app_bms_snapshot_t s_ducar_bms_snapshot;                              /* 当前G45x快照读取缓存。 */
static uint8_t s_ducar_bms_soc_pending_cmd;
static uint32_t s_ducar_bms_soc_started_ms;

/* G45x快照模块返回副本，不提供WB4工程中不存在的内部指针接口。 */
static const app_bms_snapshot_t *ducar_bms_snapshot_get(void)
{
    app_bms_snapshot_read(&s_ducar_bms_snapshot);
    return &s_ducar_bms_snapshot;
}

/* 向BMS响应数据区追加一个无符号8位字段。 */
static void ducar_bms_put_u8(uint8_t *data, uint8_t *len, uint8_t value)
{
    data[*len] = value;
    *len = (uint8_t)(*len + 1U);
}

/* 向BMS响应数据区追加一个小端无符号16位字段。 */
static void ducar_bms_put_u16(uint8_t *data, uint8_t *len, uint16_t value)
{
    data[*len] = (uint8_t)(value & 0xFFU);
    *len = (uint8_t)(*len + 1U);
    data[*len] = (uint8_t)(value >> 8);
    *len = (uint8_t)(*len + 1U);
}

/* 向BMS响应数据区追加一个小端无符号32位字段。 */
static void ducar_bms_put_u32(uint8_t *data, uint8_t *len, uint32_t value)
{
    data[*len] = (uint8_t)(value & 0xFFUL);
    *len = (uint8_t)(*len + 1U);
    data[*len] = (uint8_t)((value >> 8) & 0xFFUL);
    *len = (uint8_t)(*len + 1U);
    data[*len] = (uint8_t)((value >> 16) & 0xFFUL);
    *len = (uint8_t)(*len + 1U);
    data[*len] = (uint8_t)((value >> 24) & 0xFFUL);
    *len = (uint8_t)(*len + 1U);
}

/* 向BMS响应数据区追加一个小端有符号32位字段。 */
static void ducar_bms_put_i32(uint8_t *data, uint8_t *len, int32_t value)
{
    /* 协议定义电流按int32_t处理，线上仍按小端补码发送。 */
    ducar_bms_put_u32(data, len, (uint32_t)value);
}

/* 从BMS请求数据区读取一个小端有符号32位字段。 */
static int32_t ducar_bms_get_i32(const uint8_t *data)
{
    uint32_t u32Value;

    u32Value = ((uint32_t)data[0]) |
            ((uint32_t)data[1] << 8) |
            ((uint32_t)data[2] << 16) |
            ((uint32_t)data[3] << 24);
    return (int32_t)u32Value;
}

/* 向BMS响应数据区追加一个小端有符号16位字段。 */
static void ducar_bms_put_i16(uint8_t *data, uint8_t *len, int16_t value)
{
    /* 有符号16位字段按小端补码发送。 */
    ducar_bms_put_u16(data, len, (uint16_t)value);
}

/* 将有符号32位结果限制到协议允许的有符号16位范围。 */
/* 将有符号32位数饱和转换为有符号16位数。 */
static int16_t ducar_bms_i32_to_i16_sat(int32_t value)
{
    if (value > 32767L)
    {
        return (int16_t)32767;
    }

    if (value < (-32768L))
    {
        return (int16_t)(-32768);
    }

    return (int16_t)value;
}

/* 0x49协议电流单位是A，日志内部电流单位是10mA，因此发送前除以100。 */
/* 将日志中的电流单位转换为协议使用的安培值。 */
static int16_t ducar_bms_log_current_to_a(int16_t current_10ma)
{
    return ducar_bms_i32_to_i16_sat((int32_t)current_10ma / 100L);
}

/* 0x49协议MOS温度单位是℃，日志内部MOS温度单位是0.1℃。 */
/* 将日志中的MOS温度单位转换为协议使用的摄氏度值。 */
static int16_t ducar_bms_log_mos_temp_to_c(int16_t temp_0p1c)
{
    if ((uint16_t)temp_0p1c == 0xFFFFU)
    {
        return (int16_t)0xFFFF;
    }

    return ducar_bms_i32_to_i16_sat((int32_t)temp_0p1c / 10L);
}

/* 将无符号32位数饱和转换为无符号16位数。 */
static uint16_t ducar_bms_u32_to_u16_sat(uint32_t value)
{
    if (value > 0xFFFFUL)
    {
        return 0xFFFFU;
    }

    return (uint16_t)value;
}

/* 将有符号32位数限制并转换为无符号16位数。 */
static uint16_t ducar_bms_i32_to_u16_sat(int32_t value)
{
    if (value <= 0)
    {
        return 0U;
    }

    if (value > 0xFFFFL)
    {
        return 0xFFFFU;
    }

    return (uint16_t)value;
}

/* 将日志条数限制到协议允许的单字节范围。 */
static uint8_t ducar_bms_log_count_to_u8(uint16_t count)
{
    if (count > 0xFFU)
    {
        return 0xFFU;
    }

    return (uint8_t)count;
}

/* 获取当前快照中的有效单体数量。 */
static uint8_t ducar_bms_cell_count(const app_bms_snapshot_t *snapshot)
{
    if (snapshot == 0)
    {
        return 0U;
    }

    return (snapshot->cell_count > APP_BMS_CELL_COUNT) ? APP_BMS_CELL_COUNT : snapshot->cell_count;
}

/* 根据有效单体电压计算电池包总电压。 */
static uint32_t ducar_bms_pack_voltage_mv(const app_bms_snapshot_t *snapshot)
{
    uint32_t u32PackVoltageSumMv = 0UL;
    uint8_t u8CellCount;
    uint8_t u8CellIndex;

    u8CellCount = ducar_bms_cell_count(snapshot);
    for (u8CellIndex = 0U; u8CellIndex < u8CellCount; u8CellIndex++)
    {
        u32PackVoltageSumMv += snapshot->cell_mv[u8CellIndex];
    }

    return u32PackVoltageSumMv;
}

/* 将毫伏值转换为协议使用的10毫伏单位。 */
static uint16_t ducar_bms_mv_to_10mv(uint32_t mv)
{
    return ducar_bms_u32_to_u16_sat((mv + 5UL) / 10UL);
}

/* 将G45x业务快照中的摄氏度温度转换为0.1开尔文单位。 */
static uint16_t ducar_bms_temp_c_to_0p1k(int16_t temp_c)
{
    int32_t s32Temperature0p1k;

    /* G45x快照的电芯温度字段单位是整数℃，协议温度统一回复0.1K。 */
    s32Temperature0p1k = ((int32_t)temp_c * 10L) + 2731L;
    return ducar_bms_i32_to_u16_sat(s32Temperature0p1k);
}

/* 将MCU_NTC模块的0.1摄氏度温度转换为0.1开尔文单位。 */
static uint16_t ducar_bms_temp_0p1c_to_0p1k(int16_t temp_0p1c)
{
    return ducar_bms_i32_to_u16_sat((int32_t)temp_0p1c + 2731L);
}

/* 计算温度数组中的最大值和最小值。 */
static void ducar_bms_get_temp_extreme_0p1k(const uint16_t *temp_0p1k, uint8_t count, uint16_t *max_temp, uint16_t *min_temp)
{
    uint8_t u8TemperatureIndex;
    uint8_t bValidTemperatureFound = 0U;

    *max_temp = DUCAR_BMS_TEMP_INVALID_0P1K;
    *min_temp = DUCAR_BMS_TEMP_INVALID_0P1K;

    for (u8TemperatureIndex = 0U; u8TemperatureIndex < count; u8TemperatureIndex++)
    {
        if (temp_0p1k[u8TemperatureIndex] == DUCAR_BMS_TEMP_INVALID_0P1K)
        {
            continue;
        }

        if (bValidTemperatureFound == 0U)
        {
            *max_temp = temp_0p1k[u8TemperatureIndex];
            *min_temp = temp_0p1k[u8TemperatureIndex];
            bValidTemperatureFound = 1U;
        }
        else
        {
            if (temp_0p1k[u8TemperatureIndex] > *max_temp)
            {
                *max_temp = temp_0p1k[u8TemperatureIndex];
            }

            if (temp_0p1k[u8TemperatureIndex] < *min_temp)
            {
                *min_temp = temp_0p1k[u8TemperatureIndex];
            }
        }
    }
}

/* 将容量值转换为协议使用的无符号32位值。 */
static uint32_t ducar_bms_capacity_to_u32(long capacity_mah)
{
    if (capacity_mah <= 0L)
    {
        return 0UL;
    }

    return (uint32_t)capacity_mah;
}

/* 从BMS快照计算真实SOC百分比。 */
static uint16_t ducar_bms_soc_real_percent(const app_bms_snapshot_t *snapshot)
{
    int32_t s32RealCapacityMah;
    int32_t s32FullCapacityMah;

    if (snapshot == 0)
    {
        return 0U;
    }

    s32FullCapacityMah = snapshot->soc.s32battery_full_cap;
    s32RealCapacityMah = snapshot->soc.tsoc_capacity_data.s32real_cap_val;

    if ((s32FullCapacityMah <= 0L) || (s32RealCapacityMah <= 0L))
    {
        return 0U;
    }

    if (s32RealCapacityMah >= s32FullCapacityMah)
    {
        return 100U;
    }

    return (uint16_t)((s32RealCapacityMah * 100L) / s32FullCapacityMah);
}

/* 查找有效单体电压的最大值、最小值及其位置。 */
static void ducar_bms_get_cell_extreme(const app_bms_snapshot_t *snapshot,
                                      uint16_t *max_mv,
                                      uint16_t *min_mv,
                                      uint8_t *max_index,
                                      uint8_t *min_index)
{
    uint8_t u8CellCount;
    uint8_t u8CellIndex;

    u8CellCount = ducar_bms_cell_count(snapshot);
    if (u8CellCount == 0U)
    {
        *max_mv = 0U;
        *min_mv = 0U;
        *max_index = 0U;
        *min_index = 0U;
        return;
    }

    *max_mv = snapshot->cell_mv[0];
    *min_mv = snapshot->cell_mv[0];
    *max_index = 1U;
    *min_index = 1U;

    for (u8CellIndex = 1U; u8CellIndex < u8CellCount; u8CellIndex++)
    {
        if (snapshot->cell_mv[u8CellIndex] > *max_mv)
        {
            *max_mv = snapshot->cell_mv[u8CellIndex];
            *max_index = (uint8_t)(u8CellIndex + 1U);
        }

        if (snapshot->cell_mv[u8CellIndex] < *min_mv)
        {
            *min_mv = snapshot->cell_mv[u8CellIndex];
            *min_index = (uint8_t)(u8CellIndex + 1U);
        }
    }
}

/* 通过当前G45x UART传输通道发送BMS协议帧。 */
static bool ducar_bms_send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint16_t u16FrameLength;

    u16FrameLength = ducar_bms_pack_frame(cmd,
                                          data,
                                          len,
                                          s_ducar_bms_tx_frame,
                                          (uint16_t)sizeof(s_ducar_bms_tx_frame));
    if (u16FrameLength == 0U)
    {
        return false;
    }

    return d_uart_try_send_buffer(s_ducar_bms_tx_frame, u16FrameLength);
}

/* 发送一个带单字节ACK或NAK状态的BMS响应。 */
bool ducar_bms_send_status(uint8_t cmd, uint8_t status)
{
    return ducar_bms_send_frame(cmd, &status, 1U);
}

/* 将字符串转换为BMS响应并发送。 */
static void ducar_bms_send_string(uint8_t cmd, const char *text)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength;

    u8ResponseLength = (uint8_t)strlen(text);
    if (u8ResponseLength > DUCAR_BMS_MAX_DATA_LEN)
    {
        u8ResponseLength = DUCAR_BMS_MAX_DATA_LEN;
    }

    memcpy(au8ResponseData, text, u8ResponseLength);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送电芯温度查询响应。 */
static void ducar_bms_reply_temperature(uint8_t cmd)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength = 0U;
    uint16_t au16Temperature0p1k[MAX_TEMP_NUM];
    uint16_t u16MaximumTemperature0p1k;
    uint16_t u16MinimumTemperature0p1k;
    uint8_t u8TemperatureIndex;

    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();
    if (ptSnapshot == 0)
    {
        ducar_bms_send_frame(cmd, au8ResponseData, 0U);
        return;
    }

    /*
     * 0x05电芯温度回复MAX_TEMP_NUM路温度：
     *   MAX_TEMP_NUM = AFE_NTC_NUM + MCU_NTC_NUM。
     *   通道1：DVC1124内部AFE_NTC。
     *   通道2：原理图CELL_NTC1，对应MCU_NTC1。
     *   通道3：原理图CELL_NTC2，对应MCU_NTC2。
     * MOS_NTC由0x06单独回复，PTC_NTC后续如协议增加字段再独立挂接。
     */
    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        au16Temperature0p1k[u8TemperatureIndex] = DUCAR_BMS_TEMP_INVALID_0P1K;
        if ((ptSnapshot->temp_valid_mask & (uint8_t)(1U << u8TemperatureIndex)) != 0U)
        {
            au16Temperature0p1k[u8TemperatureIndex] =
                ducar_bms_temp_0p1c_to_0p1k(ptSnapshot->temp_0p1c[u8TemperatureIndex]);
        }
    }

    ducar_bms_get_temp_extreme_0p1k(au16Temperature0p1k,
                                    MAX_TEMP_NUM,
                                    &u16MaximumTemperature0p1k,
                                    &u16MinimumTemperature0p1k);

    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MaximumTemperature0p1k); /* DATA[0..1]：电芯温度中的最高温，单位0.1K。 */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MinimumTemperature0p1k); /* DATA[2..3]：电芯温度中的最低温，单位0.1K。 */
    for (u8TemperatureIndex = 0U;
         u8TemperatureIndex < MAX_TEMP_NUM;
         u8TemperatureIndex++)
    {
        ducar_bms_put_u16(au8ResponseData,
                          &u8ResponseLength,
                          au16Temperature0p1k[u8TemperatureIndex]); /* DATA[4..9]：温度1、温度2、温度3。 */
    }

    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送MCU温度查询响应。 */
static void ducar_bms_reply_mcu_temperature(uint8_t cmd)
{
    uint8_t au8ResponseData[2];
    uint8_t u8ResponseLength = 0U;
    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();
    uint16_t u16Temperature0p1k = DUCAR_BMS_TEMP_INVALID_0P1K;

    if ((ptSnapshot != 0) && ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_MCU) != 0U))
    {
        u16Temperature0p1k = ducar_bms_temp_0p1c_to_0p1k(ptSnapshot->ntc.mcu_temp_0p1c);
    }
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16Temperature0p1k);

    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送MOS温度查询响应。 */
static void ducar_bms_reply_mos_temperature(uint8_t cmd)
{
    uint8_t au8ResponseData[6];
    uint8_t u8ResponseLength = 0U;
    uint16_t u16MosTemperature0p1k;

    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();
    u16MosTemperature0p1k = DUCAR_BMS_TEMP_INVALID_0P1K;
    if ((ptSnapshot != 0) && ((ptSnapshot->ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U))
    {
        u16MosTemperature0p1k = ducar_bms_temp_0p1c_to_0p1k(ptSnapshot->ntc.mos_temp_0p1c);
    }

    /*
     * 当前硬件只有一路MOS_NTC，没有独立的放电MOS、充电MOS、预放MOS温度。
     * 协议需要返回三路MOS温度时，三路暂时复用同一个MOS_NTC采样值。
     */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MosTemperature0p1k);
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MosTemperature0p1k);
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MosTemperature0p1k);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送电池电压查询响应。 */
static void ducar_bms_reply_voltage(uint8_t cmd)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength = 0U;
    uint32_t u32PackVoltageMv;
    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();

    if (ptSnapshot == 0)
    {
        ducar_bms_send_frame(cmd, au8ResponseData, 0U);
        return;
    }

    u32PackVoltageMv = ducar_bms_pack_voltage_mv(ptSnapshot);

    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, ducar_bms_mv_to_10mv(u32PackVoltageMv));       /* 电池包单体累加总压，单位10mV。 */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, ducar_bms_mv_to_10mv(ptSnapshot->vbat_mv));    /* AFE VBAT采样，单位10mV。 */
//    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, ducar_bms_mv_to_10mv(ptSnapshot->bat_mv));    /* 充电口电压源暂用单体电压累加总压。 */
//    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, ducar_bms_mv_to_10mv(ptSnapshot->bat_mv));    /* 放电口电压源暂用单体电压累加总压。 */

    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送电流查询响应。 */
static void ducar_bms_reply_current(uint8_t cmd)
{
    uint8_t au8ResponseData[4];
    uint8_t u8ResponseLength = 0U;
    const app_bms_snapshot_t *ptSnapshot;

    ptSnapshot = ducar_bms_snapshot_get();

    if (ptSnapshot == 0)
    {
        ducar_bms_send_frame(cmd, au8ResponseData, 0U);
        return;
    }

    /* 协议定义为int32_t、单位mA，小端发送；正数充电，负数放电。 */
    ducar_bms_put_i32(au8ResponseData, &u8ResponseLength, ptSnapshot->current_ma);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 处理电流校准点写入请求并返回处理结果。 */
static void ducar_bms_handle_current_calibration(const ducar_bms_frame_t *frame)
{
    int32_t s32ActualCurrentMa;

    if ((frame == 0) || (frame->len != 4U))
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_CURRENT_CALIBRATION, DUCAR_BMS_NAK);
        return;
    }

    /* 0x71使用有符号实际电流：0为可选零点，正负非零值均可作为已知校准点。 */
    s32ActualCurrentMa = ducar_bms_get_i32(frame->data);
    if (app_current_calib_submit(s32ActualCurrentMa))
    {
        /* ACK只表示命令已经受理，实际采样在100ms任务中完成。 */
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_CURRENT_CALIBRATION, DUCAR_BMS_ACK);
    }
    else
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_CURRENT_CALIBRATION, DUCAR_BMS_NAK);
    }
}

/* 读取并发送当前电流校准参数。 */
static void ducar_bms_reply_current_calibration(uint8_t cmd)
{
    uint8_t au8ResponseData[8];
    uint8_t u8ResponseLength = 0U;
    uint32_t u32KFactorPpm;
    int32_t s32BiasMa;

    if (!app_current_calib_get(&u32KFactorPpm, &s32BiasMa))
    {
        ducar_bms_send_status(cmd, DUCAR_BMS_NAK);
        return;
    }

    /* 0x72协议保持8字节，返回充放电共用的唯一一组K/B。 */
    ducar_bms_put_u32(au8ResponseData, &u8ResponseLength, u32KFactorPpm);
    ducar_bms_put_i32(au8ResponseData, &u8ResponseLength, s32BiasMa);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 把快照中的抽象故障镜像还原成协议层需要的位域访问形式。 */
/* 将BMS快照中的故障和告警状态映射为协议故障字段。 */
static void ducar_bms_fault_from_snapshot(const app_bms_snapshot_t *snapshot,
                                          t_bms_fault_flag *fault)
{
    memset(fault, 0, sizeof(*fault));
    if (snapshot == 0)
    {
        return;
    }

    *fault = snapshot->fault.active.detail;
}

/* 生成并发送BMS综合状态响应。 */
static void ducar_bms_reply_status(uint8_t cmd)
{
    uint8_t au8ResponseData[16];
    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();
    t_bms_fault_flag tFaultValue;
    const t_bms_fault_flag *ptFault = &tFaultValue;

    memset(au8ResponseData, 0, sizeof(au8ResponseData));
    ducar_bms_fault_from_snapshot(ptSnapshot, &tFaultValue);

    if (ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc != 0U) { au8ResponseData[0] |= 0x80U; } /* COT-Pro：充电高温保护。 */
    if (ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_war_otc != 0U) { au8ResponseData[0] |= 0x40U; } /* COT-Alarm：充电高温告警。 */
    if ((ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 != 0U) ||
        (ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 != 0U) ||
        (ptFault->ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock != 0U) ||
        (ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc != 0U)) { au8ResponseData[0] |= 0x20U; } /* COC-Pro：充电过流/反向电流保护。 */
    if (ptFault->ucharge_fault_A.tcharge_fault_A.b1pack_war_occ != 0U) { au8ResponseData[0] |= 0x10U; } /* COC-Alarm：充电过流告警。 */
    if (ptFault->ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov != 0U) { au8ResponseData[0] |= 0x08U; } /* POV-Pro：总压过压保护。 */
    if (ptFault->ucharge_fault_B.tcharge_fault_B.b1pack_war_pov != 0U) { au8ResponseData[0] |= 0x04U; } /* POV-Alarm：总压过压告警。 */
    if ((ptFault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov != 0U) ||
        (ptFault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 != 0U)) { au8ResponseData[0] |= 0x02U; } /* COV-Pro：单体过压保护。 */
    if (ptFault->ucharge_fault_A.tcharge_fault_A.b1cell_war_cov != 0U) { au8ResponseData[0] |= 0x01U; } /* COV-Alarm：单体过压告警。 */

    if ((ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 != 0U) ||
        (ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 != 0U) ||
        (ptFault->udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock != 0U)) { au8ResponseData[2] |= 0x80U; } /* DOC-Pro3：三级、四级或放电过流锁定。 */
    if (ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 != 0U) { au8ResponseData[2] |= 0x40U; } /* DOC-Pro2：二级放电过流保护。 */
    if (ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1 != 0U) { au8ResponseData[2] |= 0x20U; } /* DOC-Pro1：一级放电过流保护。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd != 0U) { au8ResponseData[2] |= 0x10U; } /* DOC-Alarm：统一放电过流告警。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv != 0U) { au8ResponseData[2] |= 0x08U; } /* CUV-PF：深度欠压锁存。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 != 0U) { au8ResponseData[2] |= 0x04U; } /* CUV-Pro2：单体二级欠压保护。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 != 0U) { au8ResponseData[2] |= 0x02U; } /* CUV-Pro1：单体一级欠压保护。 */
    if ((ptFault->udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv != 0U) ||
        (ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv != 0U)) { au8ResponseData[2] |= 0x01U; } /* CUV-Alarm：欠压告警。 */

    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1low_soc != 0U) { au8ResponseData[3] |= 0x08U; } /* SOCLowAlarm：低SOC告警。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd != 0U) { au8ResponseData[3] |= 0x04U; } /* DOT-Pro：放电高温保护。 */
    if (ptFault->udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd != 0U) { au8ResponseData[3] |= 0x02U; } /* DOT-Alarm：放电高温告警。 */
    if ((ptFault->udischarge_fault_B.tdischarge_fault_B.b1pack_shrot != 0U) ||
        ((ptSnapshot != 0) && (ptSnapshot->afe_fault.short_circuit != 0U))) { au8ResponseData[3] |= 0x01U; } /* SCP：短路保护。 */

    if (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP != 0U) { au8ResponseData[4] |= 0x40U; } /* TMOS-Pro：MOS过温保护。 */
    if (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_waring != 0U) { au8ResponseData[4] |= 0x20U; } /* TMOS-Alarm：MOS过温告警。 */
    if ((ptFault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault != 0U) ||
        (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2 != 0U)) { au8ResponseData[4] |= 0x04U; } /* VDIF-Pro：一级或二级压差保护。 */
    if (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_waring != 0U) { au8ResponseData[4] |= 0x02U; } /* VDIF-Alarm：压差告警。 */

    if ((ptSnapshot != 0) && (ptSnapshot->ntc_abnormal_count >= BMS_NTC_FAULT_MIN_COUNT)) { au8ResponseData[5] |= 0x20U; } /* NTC-Fault：至少两路NTC电气异常。 */
    if ((ptSnapshot != 0) && (ptSnapshot->ntc_abnormal_count == 1U)) { au8ResponseData[5] |= 0x10U; } /* NTC-Warning：仅一路NTC电气异常，不执行保护。 */
    if ((ptFault->ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault != 0U) ||
        ((ptSnapshot != 0) && ((ptSnapshot->afe_fault.iic_watchdog != 0U) ||
                               (ptSnapshot->afe_fault.ntc_iic_fault != 0U)))) { au8ResponseData[5] |= 0x08U; } /* AFE-Fail：只表示AFE数据链路或通信异常。 */
    if (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure != 0U) { au8ResponseData[5] |= 0x04U; } /* DsgMOS-Fail：放电MOS失效。 */
    if (ptFault->ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure != 0U) { au8ResponseData[5] |= 0x02U; } /* ChgMOS-Fail：充电MOS失效。 */

    /* 本项目原理图没有DETD/DETC检测输入，不能把MOS路径允许状态冒充检测信号。 */
    if ((ptSnapshot != 0) && (ptSnapshot->afe_fault.charge_detect != 0U)) { au8ResponseData[6] |= 0x04U; } /* CHARGE_Signal：AFE充电器检测状态。 */

    if ((ptSnapshot != 0) && (ptSnapshot->mos.dsg_mos_state != 0U)) { au8ResponseData[7] |= 0x01U; } /* DMOS：放电MOS状态。 */
    if ((ptSnapshot != 0) && (ptSnapshot->mos.chg_mos_state != 0U)) { au8ResponseData[7] |= 0x02U; } /* CMOS：充电MOS状态。 */
    if ((ptSnapshot != 0) && (ptSnapshot->mos.pre_mos_state != 0U)) { au8ResponseData[7] |= 0x04U; } /* PreMOS：预放电MOS状态。 */

    ducar_bms_send_frame(cmd, au8ResponseData, (uint8_t)sizeof(au8ResponseData));
}

/* 生成并发送单体电压极值响应。 */
static void ducar_bms_reply_cell_extreme(uint8_t cmd)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength = 0U;
    uint16_t u16MaximumCellVoltageMv;
    uint16_t u16MinimumCellVoltageMv;
    uint8_t u8MaximumCellNumber;
    uint8_t u8MinimumCellNumber;

    ducar_bms_get_cell_extreme(ducar_bms_snapshot_get(),
                               &u16MaximumCellVoltageMv,
                               &u16MinimumCellVoltageMv,
                               &u8MaximumCellNumber,
                               &u8MinimumCellNumber);
    (void)u8MaximumCellNumber;
    (void)u8MinimumCellNumber;

    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MaximumCellVoltageMv);                    /* DATA[0..1]：最高单体电压，单位mV。 */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16MinimumCellVoltageMv);                    /* DATA[2..3]：最低单体电压，单位mV。 */
    ducar_bms_put_u16(au8ResponseData,
                      &u8ResponseLength,
                      (uint16_t)(u16MaximumCellVoltageMv - u16MinimumCellVoltageMv));/* DATA[4..5]：单体压差，单位mV。 */

    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送指定范围内的单体电压响应。 */
static void ducar_bms_reply_cell_group(uint8_t cmd, uint8_t start_index)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength = 0U;
    uint8_t u8CellCount;
    uint8_t u8CellOffset;
    uint8_t u8CellIndex;
    uint16_t u16CellVoltageMv;

    const app_bms_snapshot_t *ptSnapshot = ducar_bms_snapshot_get();
    u8CellCount = ducar_bms_cell_count(ptSnapshot);
    for (u8CellOffset = 0U; u8CellOffset < 8U; u8CellOffset++)
    {
        u8CellIndex = (uint8_t)(start_index + u8CellOffset);
        if (u8CellIndex < u8CellCount)
        {
            u16CellVoltageMv = ptSnapshot->cell_mv[u8CellIndex];
        }
        else
        {
            u16CellVoltageMv = 0U; /* 超出当前逻辑串数配置的单体按0返回。 */
        }

        ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16CellVoltageMv);
    }

    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 生成并发送SOC查询响应。 */
static void ducar_bms_reply_soc(uint8_t cmd)
{
    const app_bms_snapshot_t *ptSnapshot;
    uint8_t au8ResponseData[4];
    uint8_t u8ResponseLength = 0U;

    ptSnapshot = ducar_bms_snapshot_get();
    if (ptSnapshot == 0)
    {
        ducar_bms_send_frame(cmd, au8ResponseData, 0U);
        return;
    }

    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, (uint16_t)ptSnapshot->soc.u8display_soc); /* DATA[0..1]：显示SOC，单位%。 */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, ducar_bms_soc_real_percent(ptSnapshot));   /* DATA[2..3]：真实SOC，单位%。 */
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 发送一个无符号16位数据响应。 */
static void ducar_bms_reply_u16(uint8_t cmd, uint16_t value)
{
    uint8_t au8ResponseData[2];
    uint8_t u8ResponseLength = 0U;

    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, value);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 发送一个无符号32位数据响应。 */
static void ducar_bms_reply_u32(uint8_t cmd, uint32_t value)
{
    uint8_t au8ResponseData[4];
    uint8_t u8ResponseLength = 0U;

    ducar_bms_put_u32(au8ResponseData, &u8ResponseLength, value);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 发送指定长度的全零数据响应。 */
static void ducar_bms_reply_zero_data(uint8_t cmd, uint8_t len)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];

    /* 当前工程没有对应业务来源的协议字段，按协议长度全部填0。 */
    memset(au8ResponseData, 0, sizeof(au8ResponseData));
    ducar_bms_send_frame(cmd, au8ResponseData, len);
}

/* 生成并发送单体电压极值位置响应。 */
static void ducar_bms_reply_cell_extreme_num(uint8_t cmd)
{
    const app_bms_snapshot_t *ptSnapshot;
    uint8_t au8ResponseData[2];
    uint8_t u8ResponseLength = 0U;
    uint16_t u16MaximumCellVoltageMv;
    uint16_t u16MinimumCellVoltageMv;
    uint8_t u8MaximumCellNumber;
    uint8_t u8MinimumCellNumber;

    ptSnapshot = ducar_bms_snapshot_get();
    ducar_bms_get_cell_extreme(ptSnapshot,
                               &u16MaximumCellVoltageMv,
                               &u16MinimumCellVoltageMv,
                               &u8MaximumCellNumber,
                               &u8MinimumCellNumber);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, u8MaximumCellNumber); /* DATA[0]：最高单体电压位置，1开始。 */
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, u8MinimumCellNumber); /* DATA[1]：最低单体电压位置，1开始。 */
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 读取并发送软件序列号响应。 */
static void ducar_bms_reply_software_sn(uint8_t cmd)
{
    const char *pszSoftwareSerialNumber;
    uint8_t au8ResponseData[APP_BMS_MANUFACTURE_SN_MAX_LEN];
    uint8_t u8SerialNumberIndex;

    /* 协议0x17预留50字节；实际字符串不足时，剩余字节保持全0。 */
    memset(au8ResponseData, 0, sizeof(au8ResponseData));
    pszSoftwareSerialNumber = app_param_get_manufacture_sn();
    if (pszSoftwareSerialNumber != 0)
    {
        for (u8SerialNumberIndex = 0U;
             (u8SerialNumberIndex < sizeof(au8ResponseData)) &&
             (pszSoftwareSerialNumber[u8SerialNumberIndex] != '\0');
             u8SerialNumberIndex++)
        {
            au8ResponseData[u8SerialNumberIndex] =
                (uint8_t)pszSoftwareSerialNumber[u8SerialNumberIndex];
        }
    }

    ducar_bms_send_frame(cmd, au8ResponseData, (uint8_t)sizeof(au8ResponseData));
}

/* 处理序列号写入请求并持久化参数。 */
static void ducar_bms_handle_set_serial_number(const ducar_bms_frame_t *frame)
{
    if ((frame == 0) ||
        (frame->len == 0U) ||
        (frame->len > APP_BMS_MANUFACTURE_SN_MAX_LEN))
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_SERIAL_NUMBER, DUCAR_BMS_NAK);
        return;
    }

    if (app_param_set_manufacture_sn(frame->data, frame->len))
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_SERIAL_NUMBER, DUCAR_BMS_ACK);
    }
    else
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_SERIAL_NUMBER, DUCAR_BMS_NAK);
    }
}

/* 将版本字符串解析为协议使用的数值版本号。 */
static uint32_t ducar_bms_version_number(const char *text)
{
    uint32_t u32VersionValue = 0UL;
    uint8_t bDigitFound = 0U;

    if (text == 0)
    {
        return 0UL;
    }

    while (*text != '\0')
    {
        if ((*text >= '0') && (*text <= '9'))
        {
            u32VersionValue = (u32VersionValue * 10UL) + (uint32_t)(*text - '0');
            bDigitFound = 1U;
        }
        else if (bDigitFound != 0U)
        {
            /* 版本字符串中的第一段数字就是协议版本号。 */
            break;
        }
        text++;
    }

    return u32VersionValue;
}

/* 读取并发送软件版本响应。 */
static void ducar_bms_reply_software_version(uint8_t cmd)
{
    uint8_t au8ResponseData[4];
    uint8_t u8ResponseLength = 0U;
    uint32_t u32TVersion;
    uint32_t u32VVersion;

    u32TVersion = ducar_bms_version_number(app_param_get_package_t_version());
    u32VVersion = ducar_bms_version_number(app_param_get_package_v_version());

    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)u32TVersion);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)(u32VVersion & 0xFFUL));
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)((u32VVersion >> 8) & 0xFFUL));
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)((u32VVersion >> 16) & 0xFFUL));
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 读取并发送硬件版本响应。 */
static void ducar_bms_reply_hardware_version(uint8_t cmd)
{
    uint8_t au8ResponseData[3];
    uint8_t u8ResponseLength = 0U;
    uint32_t u32HardwareVersion;

    u32HardwareVersion = ducar_bms_version_number(app_param_get_hw_version());
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)(u32HardwareVersion & 0xFFUL));
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)((u32HardwareVersion >> 8) & 0xFFUL));
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, (uint8_t)((u32HardwareVersion >> 16) & 0xFFUL));
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 计算日志协议主体的单字节校验和。 */
static uint8_t ducar_bms_log_protocol_checksum(const uint8_t *data, uint8_t len)
{
    uint8_t u8Checksum = 0U;
    uint8_t u8DataIndex;

    for (u8DataIndex = 0U; u8DataIndex < len; u8DataIndex++)
    {
        u8Checksum = (uint8_t)(u8Checksum + data[u8DataIndex]);
    }

    return u8Checksum;
}

/*
 * 将日志内部的软件告警图转换为0x49协议告警图。
 * 协议字段和日志存储字段分开映射，避免位域排列变化影响协议格式。
 */
/* 将日志告警状态转换为协议告警位图。 */
static uint32_t ducar_bms_log_protocol_warning(const log_rtd_t *log)
{
    uint32_t u32WarningFlags;

    u32WarningFlags = 0UL;
    if (log == 0)
    {
        return u32WarningFlags;
    }

    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.dsgoc1 << 0);   /* Bit0：放电过流告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.chgoc  << 1);   /* Bit1：充电过流告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.dsgot  << 2);   /* Bit2：放电高温告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.chgot  << 3);   /* Bit3：充电高温告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.dsgut  << 4);   /* Bit4：放电低温告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.chgut  << 5);   /* Bit5：充电低温告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.mos_ot << 6);   /* Bit6：MOS过温告警。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.ntc_warn << 7); /* Bit7：NTC_Warning。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.celluv << 8);   /* Bit8：单体欠压告警，使用协议预留位。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.cellov1 << 9);  /* Bit9：单体过压告警，使用协议预留位。 */
    u32WarningFlags |= ((uint32_t)log->sw_warn.bit.unbalance << 10); /* Bit10：单体压差告警，使用协议预留位。 */

    return u32WarningFlags;
}

/* 将一条日志记录编码追加到协议数据区。 */
static void ducar_bms_append_log_protocol_body(uint8_t *data, uint8_t *len, const log_rtd_t *log)
{
    uint8_t u8BodyStartIndex;
    uint8_t u8BodyLength;
    uint8_t u8DataIndex;

    u8BodyStartIndex = *len;

    ducar_bms_put_u8(data, len, log->year);
    ducar_bms_put_u8(data, len, log->month);
    ducar_bms_put_u8(data, len, log->day);
    ducar_bms_put_u8(data, len, log->hour);
    ducar_bms_put_u8(data, len, log->min);
    ducar_bms_put_u8(data, len, log->sec);
    ducar_bms_put_u8(data, len, log->soc);
    ducar_bms_put_u16(data, len, log->maxvol);
    ducar_bms_put_u16(data, len, log->minvol);
    ducar_bms_put_i16(data, len, log->max_cell_temp);
    ducar_bms_put_i16(data, len, ducar_bms_log_current_to_a(log->cur));
    ducar_bms_put_u16(data, len, log->hw_err.status);
    /* 0x49字段顺序：AFE硬件故障(2B)、软件告警(4B)、软件故障(4B)。 */
    ducar_bms_put_u32(data, len, ducar_bms_log_protocol_warning(log));
    ducar_bms_put_u32(data, len, log->sw_err.status);

    /* 当前没有独立充电支路和辅助电源电流采样，协议字段按约定填0。 */
    ducar_bms_put_u16(data, len, 0U);
    ducar_bms_put_u16(data, len, 0U);

    ducar_bms_put_u8(data, len, log->max_cell_num);
    ducar_bms_put_u8(data, len, log->min_cell_num);
    ducar_bms_put_u16(data, len, log->packvol);
    ducar_bms_put_u8(data, len, log->max_temp_num);
    ducar_bms_put_u8(data, len, log->min_temp_num);
    ducar_bms_put_i16(data, len, log->min_cell_temp);
    ducar_bms_put_i16(data, len, ducar_bms_log_mos_temp_to_c(log->mos_temp));
    ducar_bms_put_u32(data, len, log->ex_sig.ex_sig);
    ducar_bms_put_u16(data, len, log->reset_s.reset_s);
    ducar_bms_put_u16(data, len, log->soc_s.soc_s);
    ducar_bms_put_u16(data, len, log->chg_port_vol);
    ducar_bms_put_u16(data, len, log->dsg_port_vol);
    ducar_bms_put_u8(data, len, log->sys_clk);

    for (u8DataIndex = 0U; u8DataIndex < sizeof(log->deg_data); u8DataIndex++)
    {
        ducar_bms_put_u8(data, len, log->deg_data[u8DataIndex]);
    }

    /* 协议校验只覆盖日志业务字段，不包含索引、长度和校验字节自身。 */
    u8BodyLength = (uint8_t)(*len - u8BodyStartIndex);
    ducar_bms_put_u8(data, len, ducar_bms_log_protocol_checksum(&data[u8BodyStartIndex], u8BodyLength));
}

/* CAN F232与UART 0x49共享同一日志体编码，避免两种链路字段漂移。 */
bool ducar_bms_pack_log_payload(uint16_t index, uint8_t out[60])
{
    log_rtd_t record;
    uint8_t len = 0U;
    if ((out == NULL) || (index == 0U) || !app_log_rtd_read(&record, index))
    {
        return false;
    }
    ducar_bms_put_u16(out, &len, index);
    ducar_bms_put_u8(out, &len, DUCAR_BMS_LOG_PROTOCOL_BODY_SIZE);
    ducar_bms_append_log_protocol_body(out, &len, &record);
    return (len == DUCAR_BMS_LOG_PROTOCOL_DATA_LEN);
}

/* 读取并发送RTC时间响应。 */
static void ducar_bms_reply_rtc(uint8_t cmd)
{
    uint8_t au8ResponseData[8];
    uint8_t u8ResponseLength = 0U;
    d_rtc_time_t tRtcTime;

    /* 未校时也回复RTC当前日历；硬件日历不可读时使用默认时间。 */
    if (!d_rtc_read(&tRtcTime) && !d_rtc_read_calendar(&tRtcTime))
    {
        memset(&tRtcTime, 0, sizeof(tRtcTime));
        tRtcTime.year = 2000U;
        tRtcTime.month = 1U;
        tRtcTime.day = 1U;
    }

    /* RTC读回格式：year低字节、year高字节、month、day、hour、min、sec。 */
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, tRtcTime.year);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, tRtcTime.month);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, tRtcTime.day);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, tRtcTime.hour);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, tRtcTime.min);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, tRtcTime.sec);
    ducar_bms_send_frame(cmd, au8ResponseData, u8ResponseLength);
}

/* 读取并发送有效日志条数响应。 */
static void ducar_bms_reply_log_count(void)
{
    uint8_t au8ResponseData[1];
    uint8_t u8ResponseLength = 0U;
    uint16_t u16LogCount;

    u16LogCount = app_log_rtd_num();
    ducar_bms_put_u8(au8ResponseData,
                     &u8ResponseLength,
                     ducar_bms_log_count_to_u8(u16LogCount)); /* DATA[0]：当前有效故障记录条数，协议当前按1字节返回。 */
    ducar_bms_send_frame(DUCAR_BMS_CMD_LOG_RECORD, au8ResponseData, u8ResponseLength);
}

/* 发送日志读取失败响应。 */
static void ducar_bms_reply_log_error(void)
{
    uint8_t u8NakStatus = DUCAR_BMS_NAK;

    ducar_bms_send_frame(DUCAR_BMS_CMD_LOG_RECORD, &u8NakStatus, 1U);
}

/* 校验日志导出请求并启动UART日志导出状态机。 */
static void ducar_bms_start_log_export(const ducar_bms_frame_t *frame)
{
    uint16_t u16RequestedLogCount;
    uint16_t u16ValidLogCount;

    if (frame->len != 2U) /* 读取N条故障记录：DATA[0..1]为NumSum小端。 */
    {
        ducar_bms_reply_log_error();
        return;
    }

    u16RequestedLogCount = (uint16_t)((uint16_t)frame->data[0] |
                                      ((uint16_t)frame->data[1] << 8));
    u16ValidLogCount = app_log_rtd_num();
    if ((u16RequestedLogCount == 0U) || (u16RequestedLogCount > u16ValidLogCount))
    {
        ducar_bms_reply_log_error();
        return;
    }

    s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_BUSY;
    s_ducar_bms_log_export.request_count = u16RequestedLogCount;
    s_ducar_bms_log_export.next_index = 1U;
    s_ducar_bms_log_export.last_send_ms = d_timer_get_ms() - DUCAR_BMS_LOG_EXPORT_INTERVAL_MS;
}

/* 处理UART日志条数查询和记录导出请求。 */
static void ducar_bms_handle_log_record(const ducar_bms_frame_t *frame)
{
    if ((frame->len == 1U) && (frame->data[0] == DUCAR_BMS_LOG_COUNT_REQ))
    {
        s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE; /* 重新查询总条数时，取消上一次未完成导出。 */
        ducar_bms_reply_log_count();
        return;
    }

    if (frame->len == 2U)
    {
        ducar_bms_start_log_export(frame);
        return;
    }

    ducar_bms_reply_log_error();
}

/* 处理RTC时间写入请求。 */
static void ducar_bms_handle_set_rtc(const ducar_bms_frame_t *frame)
{
    d_rtc_time_t tRtcTime;

    memset(&tRtcTime, 0, sizeof(tRtcTime));

    if (frame->len >= 7U)
    {
        tRtcTime.year = (uint16_t)((uint16_t)frame->data[0] | ((uint16_t)frame->data[1] << 8));
        tRtcTime.month = frame->data[2];
        tRtcTime.day = frame->data[3];
        tRtcTime.hour = frame->data[4];
        tRtcTime.min = frame->data[5];
        tRtcTime.sec = frame->data[6];
    }
    else if (frame->len >= 6U)
    {
        /* 兼容旧上位机的6字节格式：year为2000年后的偏移。 */
        tRtcTime.year = (uint16_t)(2000U + frame->data[0]);
        tRtcTime.month = frame->data[1];
        tRtcTime.day = frame->data[2];
        tRtcTime.hour = frame->data[3];
        tRtcTime.min = frame->data[4];
        tRtcTime.sec = frame->data[5];
    }
    else
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_RTC, DUCAR_BMS_NAK);
        return;
    }

    if (d_rtc_write(&tRtcTime))
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_RTC, DUCAR_BMS_ACK);
    }
    else
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_RTC, DUCAR_BMS_NAK);
    }
}

/* 返回当前BMS设计电压值。 */
static uint32_t ducar_bms_design_voltage_mv(void)
{
    uint32_t u32DesignVoltageMv;

    u32DesignVoltageMv = (uint32_t)APP_BMS_CELL_COUNT * DUCAR_BMS_DESIGN_CELL_MV;
    /* 0x15协议单位为1mV，直接返回整包设计电压的mV值。 */
    return u32DesignVoltageMv;
}

/* 判断BMS请求是否为无数据的简单命令。 */
static uint8_t ducar_bms_is_simple_command(const ducar_bms_frame_t *frame)
{
    return ((frame != 0) && (frame->len == 1U) && (frame->data[0] == 0U)) ? 1U : 0U;
}

/* 处理BMS关机控制请求并安排安全关机动作。 */
static void ducar_bms_handle_bms_shutdown(const ducar_bms_frame_t *frame)
{
    if (ducar_bms_is_simple_command(frame) == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_SHUTDOWN, DUCAR_BMS_NAK);
        return;
    }

    /* 关机前先保存PF锁存数据，保存失败时不能贸然切断DCDC。 */
    if (app_bms_fault_save_shutdown_pf() == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_SHUTDOWN, DUCAR_BMS_NAK);
        return;
    }

    if (ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_SHUTDOWN, DUCAR_BMS_ACK) == false)
    {
        return;
    }

    if (!d_uart_wait_tx_complete())
    {
        return;
    }
    (void)app_power_manage_request_shutdown();
}

/* 处理BMS复位控制请求并安排复位动作。 */
static void ducar_bms_handle_bms_reset(const ducar_bms_frame_t *frame)
{
    if (ducar_bms_is_simple_command(frame) == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_RESET, DUCAR_BMS_NAK);
        return;
    }

    if (app_bms_fault_save_shutdown_pf() == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_RESET, DUCAR_BMS_NAK);
        return;
    }

    if (ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_RESET, DUCAR_BMS_ACK) == false)
    {
        return;
    }

    if (!d_uart_wait_tx_complete())
    {
        return;
    }
    d_system_reset();
}

/* 处理BMS故障清除控制请求。 */
static void ducar_bms_handle_bms_fault_clear(const ducar_bms_frame_t *frame)
{
    if (ducar_bms_is_simple_command(frame) == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR, DUCAR_BMS_NAK);
        return;
    }

    /* 先向AFE下发清故障命令；IIC写入失败时不能回复ACK，也不能清除本地故障状态。 */
    if (app_hw_apply_clear_all_afe_fault() == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR, DUCAR_BMS_NAK);
        return;
    }

    /* AFE命令写入成功后，再同步擦除PF掉电记录。 */
    if (app_bms_fault_clear_shutdown_pf() == 0U)
    {
        ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR, DUCAR_BMS_NAK);
        return;
    }

    /* 清软件和汇总状态；保护配置仍保留，后续满足条件时可重新判断。 */
    app_bms_fault_clear_all();
    ducar_bms_send_status(DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR, DUCAR_BMS_ACK);
}

/* 按命令分发BMS查询、写入和控制请求，并统一返回响应。 */
void ducar_bms_handle_frame(const ducar_bms_frame_t *frame)
{
    const app_bms_snapshot_t *snapshot;

    if (frame == NULL)
    {
        return;
    }

    snapshot = ducar_bms_snapshot_get();

    switch (frame->cmd)
    {
        case DUCAR_BMS_CMD_GET_TEMPERATURE:
            ducar_bms_reply_temperature(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_MOS_TEMPERATURE:
            ducar_bms_reply_mos_temperature(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_MCU_TEMPERATURE:
            ducar_bms_reply_mcu_temperature(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_BATTERY_VOLTAGE:
            ducar_bms_reply_voltage(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_CURRENT:
            ducar_bms_reply_current(frame->cmd);
            break;

        case DUCAR_BMS_CMD_SET_CURRENT_CALIBRATION:
            ducar_bms_handle_current_calibration(frame);
            break;

        case DUCAR_BMS_CMD_GET_CURRENT_CALIBRATION:
            if ((frame->len == 1U) && (frame->data[0] == 0U))
            {
                ducar_bms_reply_current_calibration(frame->cmd);
            }
            else
            {
                ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
            }
            break;

        case DUCAR_BMS_CMD_GET_STATUS:
            ducar_bms_reply_status(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_CELL_VOLT_EXTREME:
            ducar_bms_reply_cell_extreme(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_CELL_VOLTAGE_EXTREMA_NUM:
            if (ducar_bms_is_simple_command(frame) != 0U)
            {
                ducar_bms_reply_cell_extreme_num(frame->cmd);
            }
            else
            {
                ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
            }
            break;

        case DUCAR_BMS_CMD_GET_CELLV_CMD1:
            ducar_bms_reply_cell_group(frame->cmd, 0U);
            break;

        case DUCAR_BMS_CMD_GET_CELLV_CMD2:
            ducar_bms_reply_cell_group(frame->cmd, 8U);
            break;

        case DUCAR_BMS_CMD_GET_CELLV_CMD3:
            ducar_bms_reply_cell_group(frame->cmd, 16U);
            break;

        case DUCAR_BMS_CMD_GET_SOC:
            ducar_bms_reply_soc(frame->cmd);
            break;

        case DUCAR_BMS_CMD_SOC_FORCE_OCV:
            if ((frame->len == 1U) && (frame->data[0] == 1U) &&
                (snapshot->data_ready != 0U) && (snapshot->max_cell_mv >= 2000U) &&
                (s_ducar_bms_soc_pending_cmd == DUCAR_BMS_SOC_CMD_NONE) &&
                app_bms_force_soc_ocv())
            {
                s_ducar_bms_soc_pending_cmd = frame->cmd;
                s_ducar_bms_soc_started_ms = d_timer_get_ms();
            }
            else
            {
                (void)ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
            }
            break;

        case DUCAR_BMS_CMD_SET_SOC:
            if ((frame->len == 1U) && (frame->data[0] <= 100U) &&
                (s_ducar_bms_soc_pending_cmd == DUCAR_BMS_SOC_CMD_NONE) &&
                app_bms_soc_write_request(frame->data[0]))
            {
                s_ducar_bms_soc_pending_cmd = frame->cmd;
            }
            else
            {
                (void)ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
            }
            break;

        case DUCAR_BMS_CMD_GET_SOH:
            ducar_bms_reply_u16(frame->cmd,
                               (uint16_t)ducar_bms_snapshot_get()->soc.u8display_soh);
            break;

        case DUCAR_BMS_CMD_GET_REMAIN_CAPACITY:
            ducar_bms_reply_u32(frame->cmd,
                               ducar_bms_capacity_to_u32(ducar_bms_snapshot_get()->soc.s32remain_cap));
            break;

        case DUCAR_BMS_CMD_GET_FULL_CHARGE_CAPACITY:
            ducar_bms_reply_u32(frame->cmd,
                               ducar_bms_capacity_to_u32(ducar_bms_snapshot_get()->soc.s32battery_full_cap));
            break;

        case DUCAR_BMS_CMD_GET_DESIGN_CAPACITY:
            ducar_bms_reply_u32(frame->cmd, (uint32_t)SOC_DESIGN_CAP);
            break;

        case DUCAR_BMS_CMD_GET_CYCLE_COUNT:
            ducar_bms_reply_u16(frame->cmd,
                               ducar_bms_u32_to_u16_sat(ducar_bms_snapshot_get()->soc.u32cycle_index));
            break;

        case DUCAR_BMS_CMD_GET_DESIGN_VOLTAGE:
            ducar_bms_reply_u32(frame->cmd, ducar_bms_design_voltage_mv());
            break;

        case DUCAR_BMS_CMD_GET_MANUFACTURE_DATE:
            ducar_bms_reply_zero_data(frame->cmd, 3U);
            break;

        case DUCAR_BMS_CMD_GET_SERIAL_NUMBER:
            ducar_bms_reply_software_sn(frame->cmd);
            break;

        case DUCAR_BMS_CMD_SET_SERIAL_NUMBER:
            ducar_bms_handle_set_serial_number(frame);
            break;

        case DUCAR_BMS_CMD_GET_DEVICE_NAME:
            ducar_bms_send_string(frame->cmd, app_param_get_bat_type());
            break;

        case DUCAR_BMS_CMD_GET_SOFTWARE_VERSION:
            ducar_bms_reply_software_version(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_HARDWARE_VERSION:
            ducar_bms_reply_hardware_version(frame->cmd);
            break;

        case DUCAR_BMS_CMD_GET_CUSTOMER_SW_VERSION:
            ducar_bms_reply_zero_data(frame->cmd, 4U);
            break;

        case DUCAR_BMS_CMD_GET_CUSTOMER_HW_VERSION:
            ducar_bms_reply_zero_data(frame->cmd, 3U);
            break;

        case DUCAR_BMS_CMD_GET_BATTERY_BALANCE_STAT:
            ducar_bms_reply_u32(frame->cmd, app_balance_get_mask());
            break;

        case DUCAR_BMS_CMD_SET_RTC:
            ducar_bms_handle_set_rtc(frame);
            break;

        case DUCAR_BMS_CMD_GET_RTC:
            ducar_bms_reply_rtc(frame->cmd);
            break;

        case DUCAR_BMS_CMD_SET_BMS_SHUTDOWN:
            ducar_bms_handle_bms_shutdown(frame);
            break;

        case DUCAR_BMS_CMD_SET_BMS_RESET:
            ducar_bms_handle_bms_reset(frame);
            break;

        case DUCAR_BMS_CMD_SET_BMS_FAULT_CLEAR:
            ducar_bms_handle_bms_fault_clear(frame);
            break;

        case DUCAR_BMS_CMD_LOG_RECORD:
            ducar_bms_handle_log_record(frame);
            break;

        default:
            /* 其他测试/参数类命令还没有数据接口，先明确回复NAK。 */
            ducar_bms_send_status(frame->cmd, DUCAR_BMS_NAK);
            break;
    }

}

/* 初始化BMS协议模块及UART日志状态。 */
void ducar_uart_protocol_init(void)
{
    s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE;
    s_ducar_bms_log_export.request_count = 0U;
    s_ducar_bms_log_export.next_index = 0U;
    s_ducar_bms_log_export.last_send_ms = 0UL;
    s_ducar_bms_soc_pending_cmd = DUCAR_BMS_SOC_CMD_NONE;
    s_ducar_bms_soc_started_ms = 0UL;
}

/* SOC命令由BMS任务查表并保存Flash，UART只在结果确定后回ACK/NAK。 */
static void ducar_bms_soc_reply_task_protocol(void)
{
    uint8_t cmd = s_ducar_bms_soc_pending_cmd;
    uint8_t reply = DUCAR_BMS_NAK;

    if (cmd == DUCAR_BMS_SOC_CMD_NONE)
    {
        return;
    }

    if (cmd == DUCAR_BMS_CMD_SOC_FORCE_OCV)
    {
        uint8_t save_status = app_bms_soc_ocv_save_status();

        if ((uint32_t)(d_timer_get_ms() - s_ducar_bms_soc_started_ms) >=
            DUCAR_BMS_SOC_OCV_TIMEOUT_MS)
        {
            reply = DUCAR_BMS_NAK;
        }
        else if (save_status == APP_BMS_SOC_OCV_SAVED)
        {
            reply = DUCAR_BMS_ACK;
        }
        else if (save_status != APP_BMS_SOC_OCV_SAVE_FAILED)
        {
            return;
        }
    }
    else if (cmd == DUCAR_BMS_CMD_SET_SOC)
    {
        uint8_t write_status = app_bms_soc_write_status();

        if (write_status == APP_BMS_SOC_WRITE_SUCCESS)
        {
            reply = DUCAR_BMS_ACK;
        }
        else if (write_status != APP_BMS_SOC_WRITE_FAILED)
        {
            return;
        }
    }

    if (ducar_bms_send_status(cmd, reply))
    {
        s_ducar_bms_soc_pending_cmd = DUCAR_BMS_SOC_CMD_NONE;
        if (cmd == DUCAR_BMS_CMD_SOC_FORCE_OCV)
        {
            app_bms_soc_ocv_complete();
        }
        else
        {
            app_bms_soc_write_complete();
        }
    }
}



/* 推进UART日志导出状态机并发送下一条日志。 */
static void ducar_bms_log_export_task_protocol(void)
{
    uint8_t au8ResponseData[DUCAR_BMS_MAX_DATA_LEN];
    uint8_t u8ResponseLength;
    log_rtd_t tLogRecord;
    uint16_t u16LogIndex;
    uint32_t u32CurrentTimeMs;

    if (s_ducar_bms_log_export.state != DUCAR_BMS_LOG_EXPORT_BUSY)
    {
        return;
    }

    if (ducar_iap_is_busy() || (d_uart_tx_pending() != 0U))
    {
        return;
    }

    u32CurrentTimeMs = d_timer_get_ms();
    if ((uint32_t)(u32CurrentTimeMs - s_ducar_bms_log_export.last_send_ms) < DUCAR_BMS_LOG_EXPORT_INTERVAL_MS)
    {
        return;
    }

    u16LogIndex = s_ducar_bms_log_export.next_index;
    if ((u16LogIndex == 0U) || (u16LogIndex > s_ducar_bms_log_export.request_count))
    {
        s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE;
        return;
    }

    if (!app_log_rtd_read(&tLogRecord, u16LogIndex))
    {
        ducar_bms_reply_log_error();
        s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE;
        return;
    }

    u8ResponseLength = 0U;
    ducar_bms_put_u16(au8ResponseData, &u8ResponseLength, u16LogIndex);
    ducar_bms_put_u8(au8ResponseData, &u8ResponseLength, DUCAR_BMS_LOG_PROTOCOL_BODY_SIZE);
    ducar_bms_append_log_protocol_body(au8ResponseData, &u8ResponseLength, &tLogRecord);
    if (u8ResponseLength != DUCAR_BMS_LOG_PROTOCOL_DATA_LEN)
    {
        ducar_bms_reply_log_error();
        s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE;
        return;
    }

    ducar_bms_send_frame(DUCAR_BMS_CMD_LOG_RECORD, au8ResponseData, u8ResponseLength);
    s_ducar_bms_log_export.last_send_ms = u32CurrentTimeMs;

    if (u16LogIndex >= s_ducar_bms_log_export.request_count)
    {
        s_ducar_bms_log_export.state = DUCAR_BMS_LOG_EXPORT_IDLE;
    }
    else
    {
        s_ducar_bms_log_export.next_index = (uint16_t)(u16LogIndex + 1U);
    }
}

/* 执行一次UART日志导出协议任务。 */
void ducar_bms_uart_log_task(void)
{
    ducar_bms_soc_reply_task_protocol();
    if (s_ducar_bms_soc_pending_cmd == DUCAR_BMS_SOC_CMD_NONE)
    {
        ducar_bms_log_export_task_protocol();
    }
}
