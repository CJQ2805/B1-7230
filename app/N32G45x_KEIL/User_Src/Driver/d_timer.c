/*
 * 文件作用：
 *   N32G45X定时器驱动实现，提供1ms系统节拍、1ms BMS/SOC硬件计数和软件延时判断接口。
 *
 * 主要用途：
 *   1. 驱动主循环协作式任务调度。
 *   2. 给故障延时置位/清除提供非阻塞计时。
 *   3. 给少量底层时序提供短延时接口。
 *
 * 适用范围：
 *   只适用于当前N32G45X定时器资源分配。更换MCU时保留 d_timer.h，对本文件重新实现。
 *
 * 调用关系：
 *   main.c 初始化定时器；App、Middle和Driver模块调用 d_timer_xxx()。
 *
 * 注意事项：
 *   业务代码优先使用非阻塞延时判断，避免长时间阻塞主循环导致任务时基失准。
 */
#include "Driver/d_timer.h"

#include "Driver/d_watchdog.h"
#include "misc.h"
#include "n32g45x_rcc.h"
#include "n32g45x_tim.h"
#include "FreeRTOS.h"
#include "task.h"

/* TIM6按1MHz计数，每1000个计数溢出一次，形成1ms节拍。 */
#define TIMER_COUNTER_HZ 1000000U
#define TIMER_TICK_HZ    1000U

/* TIM7按1MHz计数，每1000个计数溢出一次，形成1ms BMS/SOC计数。 */
#define BMS_TIMER_COUNTER_HZ 1000000U
#define BMS_TIMER_TICK_HZ    1000U

/* TIM6中断中递增，所有协作式定时都基于这个计数。 */
static volatile uint32_t s_timer_ms_tick = 0;

/* TIM7的1ms计数，供非阻塞延时判断使用。 */
volatile u32TimDly_t gu32_tim0_period_1ms = 0UL;

/* 调试用：记录RCC计算出的HCLK，当前144MHz配置下应为144000000。 */
volatile uint32_t gu32_timer_hclk_hz = 0U;
/* 调试用：记录RCC计算出的PCLK1，当前144MHz配置下APB1分频后应为36000000。 */
volatile uint32_t gu32_timer_pclk1_hz = 0U;
/* 调试用：记录TIM6实际采用的输入时钟，当前APB1定时器倍频后应为72000000。 */
volatile uint32_t gu32_timer_tim6_clk_hz = 0U;
/* 调试用：记录TIM7实际采用的输入时钟，当前APB1定时器倍频后应为72000000。 */
volatile uint32_t gu32_timer_tim7_clk_hz = 0U;

/* APB1分频不为1时定时器时钟会翻倍，按国民芯片定时器时钟规则计算。 */
static uint32_t d_timer_get_apb1_timer_clock(void)
{
    RCC_ClocksType clocks;
    uint32_t timer_clock;

    RCC_GetClocksFreqValue(&clocks);
    gu32_timer_hclk_hz = clocks.HclkFreq;
    gu32_timer_pclk1_hz = clocks.Pclk1Freq;

    if (clocks.Pclk1Freq == clocks.HclkFreq)
    {
        timer_clock = clocks.Pclk1Freq;
    }
    else
    {
        timer_clock = clocks.Pclk1Freq * 2U;
    }

    return timer_clock;
}

/* 配置TIM6为1ms系统调度时基。 */
static void d_timer_init_1ms_tick(void)
{
    TIM_TimeBaseInitType tim_base;
    NVIC_InitType nvic_init;
    uint32_t tim6_clock;
    uint16_t prescaler;

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_TIM6, ENABLE);

    tim6_clock = d_timer_get_apb1_timer_clock();
    gu32_timer_tim6_clk_hz = tim6_clock;
    prescaler  = (uint16_t)((tim6_clock / TIMER_COUNTER_HZ) - 1U);

    s_timer_ms_tick = 0;

    TIM_InitTimBaseStruct(&tim_base);
    tim_base.Period    = (uint16_t)((TIMER_COUNTER_HZ / TIMER_TICK_HZ) - 1U);
    tim_base.Prescaler = prescaler;
    tim_base.ClkDiv    = 0;
    tim_base.CntMode   = TIM_CNT_MODE_UP;

    TIM_InitTimeBase(TIM6, &tim_base);
    TIM_ConfigPrescaler(TIM6, prescaler, TIM_PSC_RELOAD_MODE_IMMEDIATE);
    TIM_ClrIntPendingBit(TIM6, TIM_INT_UPDATE);
    TIM_ConfigInt(TIM6, TIM_INT_UPDATE, ENABLE);

    nvic_init.NVIC_IRQChannel                   = TIM6_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority        = 1;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_Enable(TIM6, ENABLE);
}

/* 配置TIM7为1ms BMS/SOC硬件计数时基。 */
static void d_timer_init_1ms_bms_tick(void)
{
    TIM_TimeBaseInitType tim_base;
    NVIC_InitType nvic_init;
    uint32_t tim7_clock;
    uint16_t prescaler;

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_TIM7, ENABLE);

    tim7_clock = d_timer_get_apb1_timer_clock();
    gu32_timer_tim7_clk_hz = tim7_clock;
    prescaler  = (uint16_t)((tim7_clock / BMS_TIMER_COUNTER_HZ) - 1U);

    gu32_tim0_period_1ms = 0UL;

    TIM_InitTimBaseStruct(&tim_base);
    tim_base.Period    = (uint16_t)((BMS_TIMER_COUNTER_HZ / BMS_TIMER_TICK_HZ) - 1U);
    tim_base.Prescaler = prescaler;
    tim_base.ClkDiv    = 0;
    tim_base.CntMode   = TIM_CNT_MODE_UP;

    TIM_InitTimeBase(TIM7, &tim_base);
    TIM_ConfigPrescaler(TIM7, prescaler, TIM_PSC_RELOAD_MODE_IMMEDIATE);
    TIM_ClrIntPendingBit(TIM7, TIM_INT_UPDATE);
    TIM_ConfigInt(TIM7, TIM_INT_UPDATE, ENABLE);

    nvic_init.NVIC_IRQChannel                   = TIM7_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority        = 2;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);

    TIM_Enable(TIM7, ENABLE);
}

/* 定时器统一初始化入口，外部不再单独初始化BMS定时器。 */
void d_timer_init(void)
{
    d_timer_init_1ms_tick();
    d_timer_init_1ms_bms_tick();
}

/* STOP前停止应用时基，避免遗留定时器中断让WFI立即返回。 */
void d_timer_prepare_stop(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    TIM_ConfigInt(TIM6, TIM_INT_UPDATE, DISABLE);
    TIM_ConfigInt(TIM7, TIM_INT_UPDATE, DISABLE);
    TIM_Enable(TIM6, DISABLE);
    TIM_Enable(TIM7, DISABLE);
    TIM_ClrIntPendingBit(TIM6, TIM_INT_UPDATE);
    TIM_ClrIntPendingBit(TIM7, TIM_INT_UPDATE);

    NVIC_DisableIRQ(TIM6_IRQn);
    NVIC_DisableIRQ(TIM7_IRQn);
    NVIC_ClearPendingIRQ(TIM6_IRQn);
    NVIC_ClearPendingIRQ(TIM7_IRQn);
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_TIM6 | RCC_APB1_PERIPH_TIM7, DISABLE);

    __set_PRIMASK(primask);
}

/*
 * STOP唤醒后重新配置定时器，同时保留进入STOP前的单调计数。
 *
 * TIM6/TIM7在STOP期间不再产生1ms更新中断，d_timer_init()本身又会把两个
 * 软件计数器清零，因此不能直接用普通初始化替代唤醒恢复。这里先保存
 * 休眠前的值，重新配置硬件，再恢复原值；休眠期间经过的时间不计入业务时基。
 */
void d_timer_reinit_after_stop(void)
{
    uint32_t u32TimerMsBeforeStop = s_timer_ms_tick;
    u32TimDly_t bms_ms_before_stop = gu32_tim0_period_1ms;

    d_timer_init();

    s_timer_ms_tick = u32TimerMsBeforeStop;
    gu32_tim0_period_1ms = bms_ms_before_stop;
}

/* 轻量级取时函数，供任务调度、延时和PDSG计时使用。 */
uint32_t d_timer_get_ms(void)
{
    return s_timer_ms_tick;
}

/* 读取TIM7的1ms BMS/SOC计数，供保护去抖或调试查看。 */
u32TimDly_t d_timer_get_1ms(void)
{
    return gu32_tim0_period_1ms;
}

/* 取当前TIM7的1ms计数作为一次延时判断的起点。 */
u32TimDly_t d_timer_reset_count(void)
{
    return gu32_tim0_period_1ms;
}

/* 条件去抖：条件不成立时复位计时，持续足够久后返回Ok。 */
en_result_t d_timer_delay_check(bool judgement, u32TimDly_t *delay, uint32_t time_count)
{
    if ((judgement == false) || (delay == 0))
    {
        if (delay != 0)
        {
            *delay = gu32_tim0_period_1ms;
        }

        return ErrorInvalidParameter;
    }

    if (*delay == 0UL)
    {
        *delay = gu32_tim0_period_1ms;
        return ErrorInvalidParameter;
    }

    if ((uint32_t)(gu32_tim0_period_1ms - *delay) >= time_count)
    {
        *delay = gu32_tim0_period_1ms;
        return Ok;
    }

    return OperationInProgress;
}

/* 阻塞式毫秒延时，周期任务中尽量不要使用。 */
void d_timer_delay_ms(uint32_t ms)
{
    uint32_t start = d_timer_get_ms();

    while ((uint32_t)(d_timer_get_ms() - start) < ms)
    {
    }
}

void d_timer_delay_ticks(uint16_t ticks)
{
    volatile uint16_t u16DelayTimeoutCount;

    while (ticks-- > 0U)
    {
        u16DelayTimeoutCount = 36U;
        while (u16DelayTimeoutCount-- > 0U)
        {
            __NOP();
        }
    }
}

/* TIM6中断只维护毫秒计数，避免中断处理时间过长。 */
void TIM6_IRQHandler(void)
{
    if (TIM_GetIntStatus(TIM6, TIM_INT_UPDATE) != RESET)
    {
        TIM_ClrIntPendingBit(TIM6, TIM_INT_UPDATE);
        s_timer_ms_tick++;
        d_watchdog_tick_1ms();
    }
}

/* TIM7中断只推进1ms计数，故障/SOC计算放在任务上下文执行。 */
void TIM7_IRQHandler(void)
{
    if (TIM_GetIntStatus(TIM7, TIM_INT_UPDATE) != RESET)
    {
        TIM_ClrIntPendingBit(TIM7, TIM_INT_UPDATE);
        gu32_tim0_period_1ms++;
    }
}

/* FreeRTOS SysTick adapter.  Keep this boundary identical to the working
 * template: the kernel handler is called only after the scheduler starts. */
extern void xPortSysTickHandler(void);

void SysTick_Handler(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        xPortSysTickHandler();
    }
}

