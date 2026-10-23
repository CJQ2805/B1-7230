#include "boot_timer.h"

#include "misc.h"
#include "n32g45x_rcc.h"
#include "n32g45x_tim.h"

#define BOOT_TIMER_COUNTER_HZ   ((uint32_t)1000000UL)
#define BOOT_TIMER_TICK_HZ      ((uint32_t)1000UL)

static volatile uint32_t s_boot_ms_tick; /* Boot运行毫秒计数，由TIM6中断递增。 */

/* 根据APB1分频计算TIM6真实时钟，APB分频不为1时定时器时钟会翻倍。 */
static uint32_t boot_timer_get_apb1_timer_clock(void)
{
    RCC_ClocksType clocks;

    RCC_GetClocksFreqValue(&clocks);
    if (clocks.Pclk1Freq == clocks.HclkFreq)
    {
        return clocks.Pclk1Freq;
    }

    return clocks.Pclk1Freq * 2UL;
}

/* 初始化TIM6为1ms时基。 */
void boot_timer_init(void)
{
    TIM_TimeBaseInitType tim_base;
    NVIC_InitType nvic_init;
    uint32_t tim_clock;
    uint16_t prescaler;

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_TIM6, ENABLE);

    tim_clock = boot_timer_get_apb1_timer_clock();
    prescaler = (uint16_t)((tim_clock / BOOT_TIMER_COUNTER_HZ) - 1UL);
    s_boot_ms_tick = 0UL;

    TIM_InitTimBaseStruct(&tim_base);
    tim_base.Period = (uint16_t)((BOOT_TIMER_COUNTER_HZ / BOOT_TIMER_TICK_HZ) - 1UL);
    tim_base.Prescaler = prescaler;
    tim_base.ClkDiv = 0U;
    tim_base.CntMode = TIM_CNT_MODE_UP;

    TIM_InitTimeBase(TIM6, &tim_base);
    TIM_ConfigPrescaler(TIM6, prescaler, TIM_PSC_RELOAD_MODE_IMMEDIATE);
    TIM_ClrIntPendingBit(TIM6, TIM_INT_UPDATE);
    TIM_ConfigInt(TIM6, TIM_INT_UPDATE, ENABLE);

    nvic_init.NVIC_IRQChannel = TIM6_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0U;
    nvic_init.NVIC_IRQChannelSubPriority = 1U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_Enable(TIM6, ENABLE);
}

uint32_t boot_timer_get_ms(void)
{
    return s_boot_ms_tick;
}

void boot_timer_delay_ms(uint32_t ms)
{
    uint32_t start = boot_timer_get_ms();

    while ((uint32_t)(boot_timer_get_ms() - start) < ms)
    {
    }
}

/* TIM6_IRQHandler里调用，保持中断入口和驱动逻辑分开。 */
void boot_timer_irq_handler(void)
{
    if (TIM_GetIntStatus(TIM6, TIM_INT_UPDATE) != RESET)
    {
        TIM_ClrIntPendingBit(TIM6, TIM_INT_UPDATE);
        s_boot_ms_tick++;
    }
}

