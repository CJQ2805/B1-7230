#ifndef __D_TIMER_H__
#define __D_TIMER_H__

#include <stdbool.h>
#include <stdint.h>

/* 返回值风格沿用本项目既有定时器接口，故障延时逻辑可以直接复用。 */
typedef enum
{
    Ok = 0U,
    Error = 1U,
    ErrorInvalidParameter = 4U,
    OperationInProgress = 13U
} en_result_t;

typedef uint32_t u32TimDly_t;

/* BMS/SOC延时以1ms为单位，宏用于把毫秒/秒/分钟转换成计数值。 */
#define TMR0_INTERVAL      1UL
#define TMR0_DELAY_MS(ms)  ((uint32_t)(((ms) + TMR0_INTERVAL - 1UL) / TMR0_INTERVAL))
#define TMR0_DELAY_SEC(s)  (TMR0_DELAY_MS((s) * 1000UL))
#define TMR0_DELAY_MIN(m)  (TMR0_DELAY_SEC((m) * 60UL))

/* TIM7的1ms全局计数，供故障去抖和SOC计时使用。 */
extern volatile u32TimDly_t gu32_tim0_period_1ms;

/* 调试用：查看当前RCC和TIM时钟计算结果，确认1ms/10ms时基来源是否正确。 */
extern volatile uint32_t gu32_timer_hclk_hz;
extern volatile uint32_t gu32_timer_pclk1_hz;
extern volatile uint32_t gu32_timer_tim6_clk_hz;
extern volatile uint32_t gu32_timer_tim7_clk_hz;

/* 统一初始化定时器模块：TIM6和TIM7均提供1ms硬件时基。 */
void d_timer_init(void);
/* STOP前关闭TIM6/TIM7并清除待处理中断；唤醒后调用reinit恢复。 */
void d_timer_prepare_stop(void);
/* STOP唤醒后重新配置TIM6/TIM7，但不把休眠时间计入软件计数器。 */
void d_timer_reinit_after_stop(void);
/* 返回d_timer_init()之后累计的毫秒数。 */
uint32_t d_timer_get_ms(void);
/* 返回当前TIM7的1ms计数。 */
u32TimDly_t d_timer_get_1ms(void);
/* 取当前TIM7的1ms计数作为延时起点。 */
u32TimDly_t d_timer_reset_count(void);
/* 条件连续成立timecnt个1ms计数后返回Ok，用于故障置位/清除去抖。 */
en_result_t d_timer_delay_check(bool judgement, u32TimDly_t *delay, uint32_t time_count);
/* 阻塞延时，只用于初始化或非关键时序流程。 */
void d_timer_delay_ms(uint32_t ms);
void d_timer_delay_ticks(uint16_t ticks);

#endif
