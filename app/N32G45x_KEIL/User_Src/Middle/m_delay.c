/*
 * 文件作用：
 *   通用延时中间件，对上提供短延时和毫秒延时封装。
 *
 * 主要用途：
 *   1. 给软件IIC等底层时序提供短延时。
 *   2. 给少量初始化流程提供毫秒级等待。
 *
 * 适用范围：
 *   与芯片无关的中间件，实际计时来源由 Driver/d_timer 提供。
 *
 * 调用关系：
 *   m_soft_i2c、HSI_afe等模块调用本模块；本模块调用 d_timer 延时接口。
 *
 * 注意事项：
 *   业务层尽量不要使用阻塞延时，保护和任务调度应优先使用非阻塞计时。
 */
#include "Middle/m_delay.h"
#include "Driver/d_timer.h"

/* 软件IIC边沿使用的小段忙等延时。 */
void m_delay_busy(uint16_t t)
{
    d_timer_delay_ticks(t);
}

/* 毫秒延时交给TIM6系统时基实现。 */
void m_delay_ms(uint16_t ms)
{
    d_timer_delay_ms(ms);
}

/* 对外提供通用delay_ms()名称，避免上层依赖Driver层命名。 */
void delay_ms(uint16_t ms)
{
    m_delay_ms(ms);
}

/* 阻塞式秒级延时，只能在非关键周期任务外使用。 */
void m_delay_s(uint16_t s)
{
    while(s--)
    {
        m_delay_ms(1000U);
    }
}
