#ifndef _D_EXTI_H_
#define _D_EXTI_H_

#include "d_gpio.h"

/* 回调函数在EXTI挂起位清除后执行，参数带用户对象和当前引脚电平。 */
typedef void (*d_exti_callback_t)(void *object, uint8_t bit);

/* 为一个引脚配置GPIO、AFIO、EXTI和NVIC。 */
void d_exti_open(d_gpio_port_t port, d_gpio_pin_t pin, d_gpio_mode_t mode, d_gpio_pull_t pull,
                 d_exti_callback_t callback, void *object);
/* 关闭一个EXTI线，并清除已注册的回调。 */
void d_exti_close(d_gpio_pin_t pin);
/* 共用中断分发函数，由芯片中断入口调用。 */
void d_exti_irq_handler(d_gpio_pin_t pin);

#endif
