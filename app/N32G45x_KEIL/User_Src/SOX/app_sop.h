/**
 *  app_sop.h
 *  @Author: CJQ2805
 *  2022/12/27  10:56
 */ 
#ifndef __APP_SOP_H
#define __APP_SOP_H

/*
 * SOP（State of Power）按温度和 SOC 查出允许充放电电流；放电侧还带 10s/30s/持续档位。
 * 电流输入沿用项目符号：充电为正、放电为负；查表和输出的限流值以 0.1A 为单位。
 */
#include <stdint.h>
#include <stdbool.h>

/* 选择充放电 SOP 计算温度的高温分界，单位 ℃。 */
#define BMS_SOP_HIGH_TEMP   40       		// 超过此温度分界时使用最高温度选择SOP温度档，单位℃
/* 输出放电限流每改变一个0.1A档位的延时参数；调用处乘4个250ms tick，即实际间隔50秒。原行尾“ms”注释单位不准确。 */
#define BMS_CUR_RATE_OF_CHANGE  50 		// 斜率调整周期，单位秒；每周期改变0.1A

/* 放电电流持续时间档；枚举值直接用作放电表第一维的索引。 */
enum _Cur_level_e{
	
	Cur_LEVEL_10S,  // 10秒放电能力档，对应放电表第一维索引0
	Cur_LEVEL_30S,  // 30秒放电能力档，对应放电表第一维索引1
	Cur_LEVEL_Cont  // 持续放电能力档，对应放电表第一维索引2
};

/* 放电表坐标：u8cur_level 选持续时间，u8temp_level 选温度行，u8soc_level 选 SOC 列。 */
struct _bms_discharge_level_t{
   uint8_t    u8cur_level;  // 选择10秒/30秒/持续能力表面
   uint8_t    u8temp_level; // 选择放电温度区间行，索引0~9
   uint8_t    u8soc_level;  // 选择SOC区间列，索引0~10
};


/* 充电表坐标：充电表不按持续时间分档，u8cur_level 为兼容字段而保留。 */
struct _bms_charge_level_t{
   uint8_t    u8cur_level;  // 兼容共享结构体；当前充电二维表不使用该字段
   uint8_t    u8temp_level; // 选择充电温度区间行，索引0~6
   uint8_t    u8soc_level;  // 选择充电SOC区间列，索引0~10
};

/* SOP 对外状态；两个表坐标为算法中间值，电流输出单位 0.1A，电压字段单位 mV。 */
struct  _app_sop_t{
	
   struct _bms_discharge_level_t tbms_discharge_level; // 放电查表坐标/持续时间档状态
   struct _bms_charge_level_t   tbms_charge_level;     // 充电查表坐标
   int16_t   s16max_discharge_current01A;		// 最大允许放电电流，单位0.1A
   int16_t   s16max_charge_current01A;		    // 最大允许充电电流，单位0.1A
   uint16_t  u16max_recharge_current01A;        // 最大允许回充电流，单位0.1A
   uint32_t  u32max_charge_voltage;			    // 最大允许充电电压，单位mV
	
};
extern struct  _app_sop_t   gt_app_sop;
/* 输入最大/最小温度℃、SOC%、带符号电流mA和最高单体电压mV，更新全局 SOP 输出。 */
void app_sop_task(int8_t s8max_temp, int8_t s8min_temp, uint8_t u8soc, int32_t s32current, uint16_t u16max_voltage);

#endif

