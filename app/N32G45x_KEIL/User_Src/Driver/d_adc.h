#ifndef _D_ADC_H_
#define _D_ADC_H_

#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_D_ADC     1
#define Module_Minor_Version_D_ADC     0

/*
 * 文件作用：
 *   ADC驱动抽象头文件，对上层只暴露“按MCU引脚读取ADC原始值”的接口。
 *
 * 适用范围：
 *   当前工程使用N32G45X，具体ADC1/ADC2和厂家通道号只允许放在d_adc.c中。
 *   上层模块不要直接引用n32g45x_adc.h，后续换芯片时优先重写d_adc.c。
 */

/* 项目层ADC通道枚举，名称按实际MCU引脚定义，避免上层关心ADC外设实例。 */
typedef enum
{
    D_ADC_CHANNEL_PA1 = 0U,
    D_ADC_CHANNEL_PA2,
    D_ADC_CHANNEL_PA3,
    D_ADC_CHANNEL_PA4,
    D_ADC_CHANNEL_PA5,
    D_ADC_CHANNEL_PA6,
    D_ADC_CHANNEL_MCU_TEMP,
    D_ADC_CHANNEL_NUM
} d_adc_channel_t;

/* 初始化ADC1/ADC2及其时钟；内部使用HSI作为ADC时钟来源，不依赖外部晶振。 */
void d_adc_init(void);

/* 关闭ADC1/ADC2并锁止后续自动初始化，直到MCU复位。 */
void d_adc_deinit(void);

/* 低功耗暂停ADC；唤醒时通过d_adc_resume()恢复，不影响永久关闭接口。 */
void d_adc_suspend(void);
void d_adc_resume(void);

/* 读取单个ADC通道原始值，返回true表示本次采样有效，raw范围为0~4095。 */
bool d_adc_read(d_adc_channel_t channel, uint16_t *raw);

#endif

