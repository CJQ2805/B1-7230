/*
 * 文件作用：
 *   APP层MCU侧NTC采样模块，负责读取原理图中由MCU直接采样的4路NTC温度。
 *
 * 主要用途：
 *   1. 通过MCU_NTC_EN打开NTC上拉电源，再通过ADC读取MCU_NTC1、MCU_NTC2、MCU_NTC_MOS、MCU_NTC_PTC。
 *   2. 按10K上拉与10K NTC下拉分压关系，将ADC值换算为NTC阻值。
 *   3. 复用AFE使用的NTC阻温表，把阻值换算为温度，输出单位固定为0.1℃。
 *
 * 适用范围：
 *   仅适用于当前DC4803原理图中的MCU侧NTC电路。若NTC上拉阻值或分压结构改变，需要同步修改本文件换算参数。
 *
 * 调用关系：
 *   系统初始化阶段调用app_mcu_ntc_init()，DataHandle_Task以10ms周期调用app_mcu_ntc_task()。
 *   其他模块需要温度时调用app_mcu_ntc_get()读取最近一次采样快照。
 *
 * 注意事项：
 *   原理图中靠近MCU ADC脚的1K电阻是串联保护/滤波电阻，ADC输入近似无直流电流，因此阻值换算不扣除这颗1K。
 */
#include "App/app_mcu_ntc.h"

#include <stdbool.h>
#include <stdint.h>

#include "Board/board_io.h"
#include "Driver/d_adc.h"
#include "HSI_afe/hsi_ntc.h"

#define APP_MCU_NTC_PULLUP_OHM       ((uint32_t)10000U) /* 外部分压上臂电阻：10kΩ。 */
/*
 * ADC计数满量程，当前按12位ADC配置为4095；不是NTC的B值。
 * 外部NTC用它进行分压反算和ADC接近满量程的断线判定。
 */
#define APP_MCU_NTC_ADC_FULL_SCALE   ((uint32_t)4095U)
#define APP_MCU_TEMP_V30_ADDR        ((uint32_t)0x1FFFF7D0UL)
#define APP_MCU_TEMP_CAL_TEMP_0P1C   ((int32_t)300L)
#define APP_MCU_TEMP_TOFFSET_0P1C    ((int32_t)15L)
#define APP_MCU_TEMP_VDDA_MV         ((int32_t)3300L)
#define APP_MCU_TEMP_SLOPE_UV_PER_C  ((int32_t)4100L)

app_mcu_ntc_t gt_app_mcu_ntc;

/*
 * 将外部NTC通道的ADC原始值换算成NTC阻值。
 *
 * 当前硬件为“3.3V -> 10kΩ上拉 -> ADC采样点 -> NTC -> GND”。
 * 设 ADC 满量程为 F，原始值为 D，则：
 *   Vadc / Vref = D / F = Rntc / (Rpullup + Rntc)
 * 整理得到：
 *   Rntc = Rpullup * D / (F - D)
 *
 * 返回值：成功时写入以Ω为单位的阻值并返回true；参数无效或原始值位于
 * 电源轨边界时返回false。ADC脚前的1kΩ是串联保护/滤波电阻，不在直流
 * 分压支路中，因此不加入公式。
 */
static bool app_mcu_ntc_raw_to_res(uint16_t raw, uint32_t *res_ohm)
{
    uint32_t u32AdcDenominator;
    uint64_t u64ResistanceNumerator;

    /* 调用者必须提供阻值输出地址。 */
    if(res_ohm == 0)
    {
        return false;
    }

    /*
     * D=0时分压点贴近GND，对应NTC短路或采样点短路；
     * D>=F时公式分母为0或无效，对应开路/超出有效分压范围。
     * 这两类由上层记录故障，不在这里生成阻值。
     */
    if((raw == 0U) || ((uint32_t)raw >= APP_MCU_NTC_ADC_FULL_SCALE))
    {
        return false;
    }

    /* 公式分母 F-D；分母越小，反算出的NTC阻值越大。 */
    u32AdcDenominator = APP_MCU_NTC_ADC_FULL_SCALE - (uint32_t)raw;
    /* 使用64位中间量，避免10kΩ乘以ADC计数时溢出。 */
    u64ResistanceNumerator = (uint64_t)APP_MCU_NTC_PULLUP_OHM * (uint64_t)raw;
    /* 分子加分母的一半，实现正数除法的四舍五入到整数Ω。 */
    *res_ohm = (uint32_t)((u64ResistanceNumerator + (uint64_t)(u32AdcDenominator / 2U)) / (uint64_t)u32AdcDenominator);

    return true;
}

static int32_t app_mcu_ntc_div_round(int64_t numerator, int64_t denominator)
{
    if(numerator >= 0)
    {
        return (int32_t)((numerator + (denominator / 2)) / denominator);
    }

    return (int32_t)(-(((-numerator) + (denominator / 2)) / denominator));
}

/* 按N32G45X手册和厂家ADC1_TEMP例程换算内部温度，输出单位0.1℃。 */
static bool app_mcu_ntc_raw_to_mcu_temp(uint16_t raw, int16_t *temp_0p1c)
{
    uint16_t u16Calibrated30cAdcRaw;
    int32_t s32AdcRawDelta;
    int32_t s32Temperature0p1c;
    int64_t s64TemperatureNumerator;
    int64_t s64TemperatureDenominator;

    if(temp_0p1c == 0)
    {
        return false;
    }

    u16Calibrated30cAdcRaw = *((volatile const uint16_t *)APP_MCU_TEMP_V30_ADDR);
    if((raw == 0U) || ((uint32_t)raw >= APP_MCU_NTC_ADC_FULL_SCALE) ||
       (u16Calibrated30cAdcRaw == 0U) || ((uint32_t)u16Calibrated30cAdcRaw >= APP_MCU_NTC_ADC_FULL_SCALE))
    {
        return false;
    }

    /*
     * 手册公式：T = ((V30 - VSENSE) / Avg_Slope) + 30 - Toffset。
     * 厂家例程：Avg_Slope=4.1mV/℃，Toffset=1.5℃，V30为0x1FFFF7D0处的30℃校准ADC值。
     * 这里直接换算成0.1℃，避免在APP周期任务中引入浮点运算。
     */
    s32AdcRawDelta = (int32_t)u16Calibrated30cAdcRaw - (int32_t)raw;
    s64TemperatureNumerator = (int64_t)s32AdcRawDelta * (int64_t)APP_MCU_TEMP_VDDA_MV * 10000LL;
    s64TemperatureDenominator = (int64_t)APP_MCU_NTC_ADC_FULL_SCALE * (int64_t)APP_MCU_TEMP_SLOPE_UV_PER_C;
    s32Temperature0p1c = app_mcu_ntc_div_round(s64TemperatureNumerator, s64TemperatureDenominator) +
                    APP_MCU_TEMP_CAL_TEMP_0P1C - APP_MCU_TEMP_TOFFSET_0P1C;

    if(s32Temperature0p1c > 32767L)
    {
        s32Temperature0p1c = 32767L;
    }
    else if(s32Temperature0p1c < -32768L)
    {
        s32Temperature0p1c = -32768L;
    }

    *temp_0p1c = (int16_t)s32Temperature0p1c;
    return true;
}

/*
 * 采样一条外部NTC通道，并完成“ADC -> 阻值 -> 阻温表温度”转换。
 * valid_bit/fault_bit用于标记对应通道；raw、res_ohm、temp_0p1c分别输出
 * ADC计数、Ω和0.1℃。成功返回valid_bit，采样或换算失败返回0，并更新故障掩码。
 */
static uint8_t app_mcu_ntc_sample_one(d_adc_channel_t adc_channel,
                                      uint8_t valid_bit,
                                      uint8_t fault_bit,
                                      uint8_t *fault_mask,
                                      uint8_t *short_mask,
                                      uint8_t *open_mask,
                                      uint16_t *raw,
                                      uint32_t *res_ohm,
                                      int16_t *temp_0p1c)
{
    uint16_t u16AdcRaw;
    uint32_t u32NtcResistanceOhm;

    /* 这些指针由模块内部固定传入；为空时无法安全写回采样结果。 */
    if((fault_mask == 0) || (short_mask == 0) || (open_mask == 0) ||
       (raw == 0) || (res_ohm == 0) || (temp_0p1c == 0))
    {
        return 0U;
    }

    /* ADC驱动负责选择对应ADC实例、配置通道并等待一次转换完成。 */
    if(d_adc_read(adc_channel, &u16AdcRaw) == false)
    {
        *raw = 0U;
        *res_ohm = 0U;
        *temp_0p1c = 0;
        *fault_mask |= fault_bit;
        return 0U;
    }

    /* ADC为0表示NTC节点被拉到地，按NTC短路故障处理。 */
    if(u16AdcRaw == 0U)
    {
        *raw = u16AdcRaw;
        *res_ohm = 0U;
        *temp_0p1c = 0;
        *fault_mask |= fault_bit;
        *short_mask |= fault_bit;
        return 0U;
    }

    /* ADC接近满量程表示NTC断线或下拉回路断开，按NTC断线故障处理。 */
    if((uint32_t)u16AdcRaw >= APP_MCU_NTC_ADC_FULL_SCALE)
    {
        *raw = u16AdcRaw;
        *res_ohm = 0U;
        *temp_0p1c = 0;
        *fault_mask |= fault_bit;
        *open_mask |= fault_bit;
        return 0U;
    }

    if(app_mcu_ntc_raw_to_res(u16AdcRaw, &u32NtcResistanceOhm) == false)
    {
        *raw = u16AdcRaw;
        *res_ohm = 0U;
        *temp_0p1c = 0;
        *fault_mask |= fault_bit;
        return 0U;
    }

    /* 保存可追溯的原始量和阻值，再复用AFE侧同一型号NTC的阻温表。 */
    *raw = u16AdcRaw;
    *res_ohm = u32NtcResistanceOhm;
    *temp_0p1c = hsi_ntc_get_temp(gt_hsi_ntc_map_1, u32NtcResistanceOhm);

    return valid_bit;
}

static uint8_t app_mcu_ntc_sample_mcu_temp(void)
{
    uint16_t u16AdcRaw;
    int16_t s16Temperature0p1c;

    if(d_adc_read(D_ADC_CHANNEL_MCU_TEMP, &u16AdcRaw) == false)
    {
        gt_app_mcu_ntc.mcu_temp_raw = 0U;
        gt_app_mcu_ntc.mcu_temp_0p1c = 0;
        return 0U;
    }

    if(app_mcu_ntc_raw_to_mcu_temp(u16AdcRaw, &s16Temperature0p1c) == false)
    {
        gt_app_mcu_ntc.mcu_temp_raw = u16AdcRaw;
        gt_app_mcu_ntc.mcu_temp_0p1c = 0;
        return 0U;
    }

    gt_app_mcu_ntc.mcu_temp_raw = u16AdcRaw;
    gt_app_mcu_ntc.mcu_temp_0p1c = s16Temperature0p1c;

    return APP_MCU_NTC_VALID_MCU_TEMP;
}

void app_mcu_ntc_init(void)
{
    /* NTC采样电源默认应保持打开，确保上电后第一次1S任务即可读到有效温度。 */
    board_io_mcu_ntc_enable(1U);
    d_adc_init();
    gt_app_mcu_ntc.valid_mask |= app_mcu_ntc_sample_mcu_temp();
}

void app_mcu_ntc_task(void)
{
    uint8_t u8ValidNtcMask;
    uint8_t u8FaultNtcMask;
    uint8_t u8ShortNtcMask;
    uint8_t u8OpenNtcMask;

    u8ValidNtcMask = 0U;
    u8FaultNtcMask = 0U;
    u8ShortNtcMask = 0U;
    u8OpenNtcMask = 0U;

    /* 四路外部NTC共用MCU_NTC_EN；每轮采样先确认上拉电源使能。 */
    board_io_mcu_ntc_enable(1U);

    /* PA4 / ADC2：电芯NTC1。 */
    u8ValidNtcMask |= app_mcu_ntc_sample_one(D_ADC_CHANNEL_PA4,
                                         APP_MCU_NTC_VALID_NTC1,
                                         APP_MCU_NTC_FAULT_NTC1,
                                         &u8FaultNtcMask,
                                         &u8ShortNtcMask,
                                         &u8OpenNtcMask,
                                         &gt_app_mcu_ntc.ntc1_raw,
                                         &gt_app_mcu_ntc.ntc1_res_ohm,
                                         &gt_app_mcu_ntc.ntc1_0p1c);

    /* PA5 / ADC2：电芯NTC2。 */
    u8ValidNtcMask |= app_mcu_ntc_sample_one(D_ADC_CHANNEL_PA5,
                                         APP_MCU_NTC_VALID_NTC2,
                                         APP_MCU_NTC_FAULT_NTC2,
                                         &u8FaultNtcMask,
                                         &u8ShortNtcMask,
                                         &u8OpenNtcMask,
                                         &gt_app_mcu_ntc.ntc2_raw,
                                         &gt_app_mcu_ntc.ntc2_res_ohm,
                                         &gt_app_mcu_ntc.ntc2_0p1c);

    /* PA3 / ADC1：MOS NTC。 */
    u8ValidNtcMask |= app_mcu_ntc_sample_one(D_ADC_CHANNEL_PA3,
                                         APP_MCU_NTC_VALID_MOS,
                                         APP_MCU_NTC_FAULT_MOS,
                                         &u8FaultNtcMask,
                                         &u8ShortNtcMask,
                                         &u8OpenNtcMask,
                                         &gt_app_mcu_ntc.mos_raw,
                                         &gt_app_mcu_ntc.mos_res_ohm,
                                         &gt_app_mcu_ntc.mos_0p1c);

    /* PA2 / ADC1：PTC NTC。 */
    u8ValidNtcMask |= app_mcu_ntc_sample_one(D_ADC_CHANNEL_PA2,
                                         APP_MCU_NTC_VALID_PTC,
                                         APP_MCU_NTC_FAULT_PTC,
                                         &u8FaultNtcMask,
                                         &u8ShortNtcMask,
                                         &u8OpenNtcMask,
                                         &gt_app_mcu_ntc.ptc_raw,
                                         &gt_app_mcu_ntc.ptc_res_ohm,
                                         &gt_app_mcu_ntc.ptc_0p1c);

    /* 本任务还会刷新内部温度；本次注释说明的四路外部NTC在上面已采样完毕。 */
    u8ValidNtcMask |= app_mcu_ntc_sample_mcu_temp();

    /* 每轮先清零再汇总：仅成功通道置valid位，故障通道分别置故障/短路/断线位。 */
    gt_app_mcu_ntc.valid_mask = u8ValidNtcMask;
    gt_app_mcu_ntc.fault_mask = u8FaultNtcMask;
    gt_app_mcu_ntc.short_mask = u8ShortNtcMask;
    gt_app_mcu_ntc.open_mask = u8OpenNtcMask;
    gt_app_mcu_ntc.all_valid = ((u8ValidNtcMask & APP_MCU_NTC_VALID_ALL) == APP_MCU_NTC_VALID_ALL) ? 1U : 0U;
}

const app_mcu_ntc_t *app_mcu_ntc_get(void)
{
    return &gt_app_mcu_ntc;
}

