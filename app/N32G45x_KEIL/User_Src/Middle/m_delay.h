#ifndef _M_DELAY_H_
#define _M_DELAY_H_

#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_M_Delay    0
#define Module_Minor_Version_M_Delay    0

/* 旧软件模拟驱动使用的粗略忙等常量。 */
typedef enum
{
    M_DELAY_5US = 1,
    M_DELAY_10US = 2,
    M_DELAY_20US = 5,
    M_DELAY_100US = 30,
    M_DELAY_1MS = 315,
    M_DELAY_5MS = 1600,
    M_DELAY_10MS = 3200,
    M_DELAY_50MS = 16000,
    M_DELAY_100MS = 32000
} m_delay_time_t;

/* 短忙等延时，精度依赖CPU主频，仅用于位级时序。 */
void m_delay_busy(uint16_t t);
/* 基于TIM6时基的毫秒延时。 */
void m_delay_ms(uint16_t ms);
/* 对上层提供统一命名的毫秒阻塞延时接口，内部复用m_delay_ms()。 */
void delay_ms(uint16_t ms);
/* 基于m_delay_ms()的阻塞秒级延时。 */
void m_delay_s(uint16_t s);

#endif
