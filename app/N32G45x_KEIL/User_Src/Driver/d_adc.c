/*
 * 文件作用：
 *   N32G45X ADC驱动适配层，把项目层ADC通道转换为厂家ADC1/ADC2通道并完成单次采样。
 *
 * 主要用途：
 *   1. 给APP层的MCU NTC、板载电压采样等模块提供统一ADC读取接口。
 *   2. 把ADC时钟、校准、通道选择、转换等待等厂家库细节集中封装在本文件。
 *   3. 所有等待都带超时，避免ADC异常时卡死主循环。
 *
 * 适用范围：
 *   仅适用于当前N32G45X芯片。后续如果更换MCU，上层保持d_adc.h接口不变，重写本文件即可。
 *
 * 注意事项：
 *   当前硬件使用内部时钟，ADC时钟按原厂例程要求选择HSI，不使用HSE。
 */
#include "Driver/d_adc.h"

#include "n32g45x_adc.h"
#include "n32g45x_rcc.h"
#include "FreeRTOS.h"
#include "task.h"

#define D_ADC_TIMEOUT_COUNT       ((uint32_t)100000UL)
#define D_ADC_RAW_MASK            ((uint16_t)0x0FFFU)

typedef struct
{
    ADC_Module *adc;
    uint8_t channel;
} d_adc_hw_channel_t;

static bool s_d_adc_ready = false;
static volatile uint8_t s_d_adc_shutdown_latched;
static volatile uint8_t s_d_adc_suspended;
static volatile uint8_t s_d_adc_active_reads;

/* d_adc_deinit()只在BMS任务中调用；等待已开始的采样退出后再关闭外设。 */
static void d_adc_read_exit(void)
{
    taskENTER_CRITICAL();
    if(s_d_adc_active_reads != 0U)
    {
        s_d_adc_active_reads--;
    }
    taskEXIT_CRITICAL();
}

/* 将项目层通道映射到N32G45X实际ADC外设和厂家通道号。 */
static bool d_adc_channel_to_hw(d_adc_channel_t channel, d_adc_hw_channel_t *hw)
{
    if(hw == 0)
    {
        return false;
    }

    switch(channel)
    {
        case D_ADC_CHANNEL_PA1:
            hw->adc = ADC1;
            hw->channel = ADC1_Channel_02_PA1;
            break;

        case D_ADC_CHANNEL_PA2:
            hw->adc = ADC1;
            hw->channel = ADC1_Channel_11_PA2;
            break;

        case D_ADC_CHANNEL_PA3:
            hw->adc = ADC1;
            hw->channel = ADC1_Channel_04_PA3;
            break;

        case D_ADC_CHANNEL_PA4:
            hw->adc = ADC2;
            hw->channel = ADC2_Channel_01_PA4;
            break;

        case D_ADC_CHANNEL_PA5:
            hw->adc = ADC2;
            hw->channel = ADC2_Channel_02_PA5;
            break;

        case D_ADC_CHANNEL_PA6:
            hw->adc = ADC1;
            hw->channel = ADC1_Channel_03_PA6;
            break;

        case D_ADC_CHANNEL_MCU_TEMP:
            hw->adc = ADC1;
            hw->channel = ADC_CH_TEMP_SENSOR;
            break;

        default:
            return false;
    }

    return true;
}

/* 等待ADC进入就绪状态，失败时返回false，避免初始化阶段死等。 */
static bool d_adc_wait_ready(ADC_Module *adc)
{
    uint32_t u32TimeoutCount;

    u32TimeoutCount = D_ADC_TIMEOUT_COUNT;
    while(ADC_GetFlagStatusNew(adc, ADC_FLAG_RDY) == RESET)
    {
        if(u32TimeoutCount == 0U)
        {
            return false;
        }
        u32TimeoutCount--;
    }

    return true;
}

/* ADC上电后执行一次校准，校准超时则认为该ADC不可用。 */
static bool d_adc_calibrate(ADC_Module *adc)
{
    uint32_t u32TimeoutCount;

    ADC_StartCalibration(adc);

    u32TimeoutCount = D_ADC_TIMEOUT_COUNT;
    while(ADC_GetCalibrationStatus(adc) != RESET)
    {
        if(u32TimeoutCount == 0U)
        {
            return false;
        }
        u32TimeoutCount--;
    }

    return true;
}

/* 配置单个ADC外设为软件触发、单通道、单次转换模式。 */
static void d_adc_config_single_mode(ADC_Module *adc)
{
    ADC_InitType adc_init;

    ADC_InitStruct(&adc_init);
    adc_init.WorkMode       = ADC_WORKMODE_INDEPENDENT;
    adc_init.MultiChEn      = DISABLE;
    adc_init.ContinueConvEn = DISABLE;
    adc_init.ExtTrigSelect  = ADC_EXT_TRIGCONV_NONE;
    adc_init.DatAlign       = ADC_DAT_ALIGN_R;
    adc_init.ChsNumber      = 1U;
    ADC_Init(adc, &adc_init);
}

void d_adc_init(void)
{
    if((s_d_adc_shutdown_latched != 0U) || (s_d_adc_suspended != 0U))
    {
        return;
    }

    if(s_d_adc_ready != false)
    {
        return;
    }

    /* ADC1/ADC2共用ADC时钟配置；当前工程使用HSI，因此不能选择HSE作为ADC1MCLK。 */
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_ADC1 | RCC_AHB_PERIPH_ADC2, ENABLE);
    ADC_ConfigClk(ADC_CTRL3_CKMOD_AHB, RCC_ADCHCLK_DIV16);
    RCC_ConfigAdc1mClk(RCC_ADC1MCLK_SRC_HSI, RCC_ADC1MCLK_DIV8);

    d_adc_config_single_mode(ADC1);
    d_adc_config_single_mode(ADC2);

    /* 内部温度传感器使用ADC1_IN16，读取前必须打开TEMP/VREFINT通道。 */
    ADC_EnableTempSensorVrefint(ENABLE);

    ADC_Enable(ADC1, ENABLE);
    if(d_adc_wait_ready(ADC1) == false)
    {
        return;
    }
    if(d_adc_calibrate(ADC1) == false)
    {
        return;
    }

    ADC_Enable(ADC2, ENABLE);
    if(d_adc_wait_ready(ADC2) == false)
    {
        return;
    }
    if(d_adc_calibrate(ADC2) == false)
    {
        return;
    }

    s_d_adc_ready = true;
}

void d_adc_deinit(void)
{
    taskENTER_CRITICAL();
    s_d_adc_shutdown_latched = 1U;
    taskEXIT_CRITICAL();

    /* 关机任务让正在进行的单次转换自然结束，避免在ADC读函数中途关时钟。 */
    while(s_d_adc_active_reads != 0U)
    {
        vTaskDelay(1U);
    }

    ADC_Enable(ADC1, DISABLE);
    ADC_Enable(ADC2, DISABLE);
    ADC_EnableTempSensorVrefint(DISABLE);
    RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_ADC1 | RCC_AHB_PERIPH_ADC2, DISABLE);
    s_d_adc_ready = false;
}

void d_adc_suspend(void)
{
    taskENTER_CRITICAL();
    s_d_adc_suspended = 1U;
    taskEXIT_CRITICAL();

    /* 让已开始的单次采样在调度器仍运行时完成，再关ADC和时钟。 */
    while(s_d_adc_active_reads != 0U)
    {
        vTaskDelay(1U);
    }

    if(s_d_adc_ready != false)
    {
        ADC_Enable(ADC1, DISABLE);
        ADC_Enable(ADC2, DISABLE);
        ADC_EnableTempSensorVrefint(DISABLE);
        RCC_EnableAHBPeriphClk(RCC_AHB_PERIPH_ADC1 | RCC_AHB_PERIPH_ADC2, DISABLE);
        s_d_adc_ready = false;
    }
}

void d_adc_resume(void)
{
    if(s_d_adc_shutdown_latched != 0U)
    {
        return;
    }

    taskENTER_CRITICAL();
    s_d_adc_suspended = 0U;
    taskEXIT_CRITICAL();
    d_adc_init();
}

bool d_adc_read(d_adc_channel_t channel, uint16_t *raw)
{
    d_adc_hw_channel_t hw;
    uint32_t u32TimeoutCount;
    bool temp_sensor_channel;

    if(raw == 0)
    {
        return false;
    }

    if((s_d_adc_shutdown_latched != 0U) || (s_d_adc_suspended != 0U))
    {
        return false;
    }

    if(s_d_adc_ready == false)
    {
        d_adc_init();
    }

    if((s_d_adc_ready == false) || (d_adc_channel_to_hw(channel, &hw) == false))
    {
        return false;
    }

    taskENTER_CRITICAL();
    if((s_d_adc_shutdown_latched != 0U) || (s_d_adc_suspended != 0U) ||
       (s_d_adc_ready == false))
    {
        taskEXIT_CRITICAL();
        return false;
    }
    s_d_adc_active_reads++;
    taskEXIT_CRITICAL();

    temp_sensor_channel = (channel == D_ADC_CHANNEL_MCU_TEMP) ? true : false;
    if(temp_sensor_channel != false)
    {
        /* 内部温度传感器平时不参与外部NTC采样，按需打开可降低平均功耗。 */
        ADC_EnableTempSensorVrefint(ENABLE);
    }

    /* 清掉上一次转换遗留标志，再启动本次单通道转换。 */
    ADC_ClearFlag(hw.adc, ADC_FLAG_ENDCA);
    ADC_ClearFlag(hw.adc, ADC_FLAG_STR);
    ADC_ConfigRegularChannel(hw.adc, hw.channel, 1U, ADC_SAMP_TIME_239CYCLES5);
    ADC_EnableSoftwareStartConv(hw.adc, ENABLE);

    u32TimeoutCount = D_ADC_TIMEOUT_COUNT;
    while(ADC_GetFlagStatus(hw.adc, ADC_FLAG_ENDCA) == RESET)
    {
        if(u32TimeoutCount == 0U)
        {
            if(temp_sensor_channel != false)
            {
                ADC_EnableTempSensorVrefint(DISABLE);
            }
            d_adc_read_exit();
            return false;
        }
        u32TimeoutCount--;
    }

    ADC_ClearFlag(hw.adc, ADC_FLAG_ENDCA);
    ADC_ClearFlag(hw.adc, ADC_FLAG_STR);
    *raw = (uint16_t)(ADC_GetDat(hw.adc) & D_ADC_RAW_MASK);

    if(temp_sensor_channel != false)
    {
        ADC_EnableTempSensorVrefint(DISABLE);
    }

    d_adc_read_exit();
    return true;
}

