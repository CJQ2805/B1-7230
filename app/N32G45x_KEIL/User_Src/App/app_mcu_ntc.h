#ifndef _APP_MCU_NTC_H_
#define _APP_MCU_NTC_H_

#include <stdint.h>

#define Module_Major_Version_APP_MCU_NTC     1
#define Module_Minor_Version_APP_MCU_NTC     0

/*
 * 文件作用：
 *   MCU侧NTC采样管理模块，对外输出4路NTC温度、ADC原始值和换算阻值。
 *
 * 适用范围：
 *   当前DC4803硬件的MCU_NTC1、MCU_NTC2、MCU_NTC_MOS、MCU_NTC_PTC四路采样。
 *   NTC阻值表复用AFE温度使用的TDK NTCG163JF103FT1S表。
 */

#define APP_MCU_NTC_VALID_NTC1      ((uint8_t)0x01U)
#define APP_MCU_NTC_VALID_NTC2      ((uint8_t)0x02U)
#define APP_MCU_NTC_VALID_MOS       ((uint8_t)0x04U)
#define APP_MCU_NTC_VALID_PTC       ((uint8_t)0x08U)
#define APP_MCU_NTC_VALID_MCU_TEMP  ((uint8_t)0x10U)
#define APP_MCU_NTC_VALID_ALL       ((uint8_t)0x0FU)

/* 外部MCU_NTC故障位：短路、断线、ADC读取失败都归入对应通道故障。 */
#define APP_MCU_NTC_FAULT_NTC1      ((uint8_t)0x01U)
#define APP_MCU_NTC_FAULT_NTC2      ((uint8_t)0x02U)
#define APP_MCU_NTC_FAULT_MOS       ((uint8_t)0x04U)
#define APP_MCU_NTC_FAULT_PTC       ((uint8_t)0x08U)

typedef struct
{
    int16_t ntc1_0p1c;       /* MCU_NTC1温度，单位0.1℃。 */
    int16_t ntc2_0p1c;       /* MCU_NTC2温度，单位0.1℃。 */
    int16_t mos_0p1c;        /* MCU_NTC_MOS温度，单位0.1℃。 */
    int16_t ptc_0p1c;        /* MCU_NTC_PTC温度，单位0.1℃。 */
    int16_t mcu_temp_0p1c;   /* MCU内部温度，单位0.1℃。 */

    uint16_t ntc1_raw;       /* MCU_NTC1的12位ADC原始值。 */
    uint16_t ntc2_raw;       /* MCU_NTC2的12位ADC原始值。 */
    uint16_t mos_raw;        /* MCU_NTC_MOS的12位ADC原始值。 */
    uint16_t ptc_raw;        /* MCU_NTC_PTC的12位ADC原始值。 */
    uint16_t mcu_temp_raw;   /* MCU内部温度传感器ADC1_IN16的12位原始值。 */

    uint32_t ntc1_res_ohm;   /* MCU_NTC1换算阻值，单位Ω。 */
    uint32_t ntc2_res_ohm;   /* MCU_NTC2换算阻值，单位Ω。 */
    uint32_t mos_res_ohm;    /* MCU_NTC_MOS换算阻值，单位Ω。 */
    uint32_t ptc_res_ohm;    /* MCU_NTC_PTC换算阻值，单位Ω。 */

    uint8_t valid_mask;      /* 每一路采样有效标志，bit定义见APP_MCU_NTC_VALID_xxx。 */
    uint8_t fault_mask;      /* 每一路NTC故障标志，bit定义见APP_MCU_NTC_FAULT_xxx。 */
    uint8_t short_mask;      /* NTC短路标志，ADC接近0表示短路。 */
    uint8_t open_mask;       /* NTC断线/上拉端异常标志，ADC接近满量程表示断线。 */
    uint8_t all_valid;       /* 四路都有效时为1，否则为0。 */
} app_mcu_ntc_t;

/* 调试重点：Watch窗口直接查看该结构体，温度字段单位均为0.1℃。 */
extern app_mcu_ntc_t gt_app_mcu_ntc;

/* 初始化MCU侧NTC采样模块，打开NTC采样使能并初始化ADC。 */
void app_mcu_ntc_init(void);

/* 1S任务接口；完成四路NTC采样、阻值换算和温度换算。 */
void app_mcu_ntc_task(void);

/* 获取最近一次MCU侧NTC采样结果，返回内部只读快照地址。 */
const app_mcu_ntc_t *app_mcu_ntc_get(void);

#endif
