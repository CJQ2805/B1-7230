/*
 * 文件作用：
 *   N32G45X外部中断驱动实现，将Driver定义的EXTI抽象接口转换成N32具体配置。
 *
 * 主要用途：
 *   1. 根据抽象GPIO端口和引脚打开对应EXTI线。
 *   2. 管理中断回调函数和用户对象。
 *   3. 在中断入口中分发板级外部事件。
 *
 * 适用范围：
 *   只适用于当前N32G45X芯片。更换MCU时保留 d_exti.h，对本文件重新实现。
 *
 * 调用关系：
 *   HSI_afe、低功耗等模块通过 d_exti_open() 注册回调；n32g45x_it.c 调用驱动中断处理函数。
 *
 * 注意事项：
 *   中断中只置位或分发轻量事件，不允许执行IIC、Flash或复杂业务逻辑。
 */
#include "Driver/d_exti.h"
#include "n32g45x.h"

typedef struct
{
    d_gpio_port_t    port;
    d_gpio_pin_t     pin;
    d_exti_callback_t callback;
    void            *object;
} d_exti_map_t;

static d_exti_map_t s_exti_map[16];

static bool d_exti_is_valid_port(d_gpio_port_t port)
{
    switch(port)
    {
        case D_GPIO_PORT_A:
        case D_GPIO_PORT_B:
        case D_GPIO_PORT_C:
        case D_GPIO_PORT_D:
        case D_GPIO_PORT_E:
            return true;
        default:
            return false;
    }
}

static bool d_exti_is_single_pin(d_gpio_pin_t pin)
{
    return (pin != 0U) && ((pin & (d_gpio_pin_t)(pin - 1U)) == 0U);
}

static int8_t d_exti_get_pin_index(d_gpio_pin_t pin)
{
    for(int8_t s8PinIndex = 0; s8PinIndex < 16; s8PinIndex++)
    {
        if((pin & (d_gpio_pin_t)(1UL << (uint8_t)s8PinIndex)) != 0U)
        {
            return s8PinIndex;
        }
    }

    return -1;
}

static uint8_t d_exti_get_port_source(d_gpio_port_t port)
{
    switch(port)
    {
        case D_GPIO_PORT_A: return GPIOA_PORT_SOURCE;
        case D_GPIO_PORT_B: return GPIOB_PORT_SOURCE;
        case D_GPIO_PORT_C: return GPIOC_PORT_SOURCE;
        case D_GPIO_PORT_D: return GPIOD_PORT_SOURCE;
        case D_GPIO_PORT_E: return GPIOE_PORT_SOURCE;
        default: return 0xffU;
    }
}

static uint32_t d_exti_get_exti_line(d_gpio_pin_t pin)
{
    int8_t s8PinIndex = d_exti_get_pin_index(pin);

    if(s8PinIndex < 0)
    {
        return 0U;
    }

    return (1UL << (uint8_t)s8PinIndex);
}

static IRQn_Type d_exti_get_irq(d_gpio_pin_t pin)
{
    int8_t s8PinIndex = d_exti_get_pin_index(pin);

    switch(s8PinIndex)
    {
        case 0: return EXTI0_IRQn;
        case 1: return EXTI1_IRQn;
        case 2: return EXTI2_IRQn;
        case 3: return EXTI3_IRQn;
        case 4: return EXTI4_IRQn;
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
            return EXTI9_5_IRQn;
        case 10:
        case 11:
        case 12:
        case 13:
        case 14:
        case 15:
            return EXTI15_10_IRQn;
        default:
            return EXTI0_IRQn;
    }
}

static EXTI_TriggerType d_exti_get_trigger(d_gpio_mode_t mode)
{
    switch(mode)
    {
        case d_GPIO_MODE_IT_RISING:
            return EXTI_Trigger_Rising;
        case d_GPIO_MODE_IT_FALLING:
            return EXTI_Trigger_Falling;
        case d_GPIO_MODE_IT_RISING_FALLING:
            return EXTI_Trigger_Rising_Falling;
        default:
            return EXTI_Trigger_Falling;
    }
}

void d_exti_open(d_gpio_port_t port, d_gpio_pin_t pin, d_gpio_mode_t mode, d_gpio_pull_t pull,
                 d_exti_callback_t callback, void *object)
{
    EXTI_InitType exti_init;
    NVIC_InitType nvic_init;
    int8_t        s8PinIndex;
    uint8_t       port_source;
    uint32_t      u32ExtiLine;
    IRQn_Type     irq;

    if(!d_exti_is_single_pin(pin) || !d_exti_is_valid_port(port))
    {
        return;
    }

    s8PinIndex = d_exti_get_pin_index(pin);
    if(s8PinIndex < 0)
    {
        return;
    }

    port_source = d_exti_get_port_source(port);
    u32ExtiLine = d_exti_get_exti_line(pin);
    irq         = d_exti_get_irq(pin);
    if((port_source == 0xffU) || (u32ExtiLine == 0U))
    {
        return;
    }

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
    d_gpio_init(port, pin, mode, pull);
    GPIO_ConfigEXTILine(port_source, (uint8_t)s8PinIndex);

    EXTI_ClrITPendBit(u32ExtiLine);
    EXTI_InitStruct(&exti_init);
    exti_init.EXTI_Line    = u32ExtiLine;
    exti_init.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti_init.EXTI_Trigger = d_exti_get_trigger(mode);
    exti_init.EXTI_LineCmd = ENABLE;
    EXTI_InitPeripheral(&exti_init);

    s_exti_map[s8PinIndex].port     = port;
    s_exti_map[s8PinIndex].pin      = pin;
    s_exti_map[s8PinIndex].callback = callback;
    s_exti_map[s8PinIndex].object   = object;

    nvic_init.NVIC_IRQChannel                   = irq;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init.NVIC_IRQChannelSubPriority        = 0;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);
}

void d_exti_close(d_gpio_pin_t pin)
{
    EXTI_InitType exti_init;
    int8_t        s8PinIndex;
    uint32_t      u32ExtiLine;

    if(!d_exti_is_single_pin(pin))
    {
        return;
    }

    s8PinIndex = d_exti_get_pin_index(pin);
    if(s8PinIndex < 0)
    {
        return;
    }

    u32ExtiLine = d_exti_get_exti_line(pin);
    if(u32ExtiLine == 0U)
    {
        return;
    }

    EXTI_InitStruct(&exti_init);
    exti_init.EXTI_Line    = u32ExtiLine;
    exti_init.EXTI_Mode    = EXTI_Mode_Interrupt;
    exti_init.EXTI_Trigger = EXTI_Trigger_Falling;
    exti_init.EXTI_LineCmd = DISABLE;
    EXTI_InitPeripheral(&exti_init);
    EXTI_ClrITPendBit(u32ExtiLine);

    s_exti_map[s8PinIndex].port     = D_GPIO_PORT_INVALID;
    s_exti_map[s8PinIndex].pin      = 0U;
    s_exti_map[s8PinIndex].callback = 0;
    s_exti_map[s8PinIndex].object   = 0;
}

void d_exti_irq_handler(d_gpio_pin_t pin)
{
    int8_t   s8PinIndex;
    uint8_t  u8PinState;
    uint32_t u32ExtiLine;

    if(!d_exti_is_single_pin(pin))
    {
        return;
    }

    s8PinIndex = d_exti_get_pin_index(pin);
    if(s8PinIndex < 0)
    {
        return;
    }

    u32ExtiLine = d_exti_get_exti_line(pin);
    if(u32ExtiLine == 0U)
    {
        return;
    }

    if(EXTI_GetITStatus(u32ExtiLine) == RESET)
    {
        return;
    }

    EXTI_ClrITPendBit(u32ExtiLine);

    if(s_exti_map[s8PinIndex].callback != 0)
    {
        u8PinState = d_gpio_get_state(s_exti_map[s8PinIndex].port, s_exti_map[s8PinIndex].pin);
        s_exti_map[s8PinIndex].callback(s_exti_map[s8PinIndex].object, u8PinState);
    }
}

