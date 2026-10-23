/*
 * 文件作用：
 *   N32G45X GPIO驱动实现，将项目抽象端口和引脚转换成N32具体GPIO寄存器操作。
 *
 * 主要用途：
 *   1. 初始化输入、输出、模拟、复用和中断相关GPIO模式。
 *   2. 设置和读取GPIO电平。
 *   3. 为Board、Middle软件IIC和其他底层适配模块提供统一GPIO能力。
 *
 * 适用范围：
 *   只适用于当前N32G45X芯片。更换MCU时保留 d_gpio.h，对本文件重新实现。
 *
 * 调用关系：
 *   Board、Middle、HSI和少量驱动适配层调用 d_gpio_xxx()；本文件内部调用N32 GPIO/RCC接口。
 *
 * 注意事项：
 *   业务模块不应直接组合端口和引脚，原理图信号统一放在Board层。
 */
#include "Driver/d_gpio.h"
#include "n32g45x.h"

static GPIO_Module *d_gpio_get_module(d_gpio_port_t port)
{
    switch(port)
    {
        case D_GPIO_PORT_A: return GPIOA;
        case D_GPIO_PORT_B: return GPIOB;
        case D_GPIO_PORT_C: return GPIOC;
        case D_GPIO_PORT_D: return GPIOD;
        case D_GPIO_PORT_E: return GPIOE;
        default: return 0;
    }
}

static void d_gpio_enable_clock(d_gpio_port_t port)
{
    switch(port)
    {
        case D_GPIO_PORT_A:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOA, ENABLE);
            break;
        case D_GPIO_PORT_B:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOB, ENABLE);
            break;
        case D_GPIO_PORT_C:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOC, ENABLE);
            break;
        case D_GPIO_PORT_D:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOD, ENABLE);
            break;
        case D_GPIO_PORT_E:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOE, ENABLE);
            break;
        default:
            break;
    }
}

void d_gpio_set_debug_mode(d_gpio_debug_mode_t mode)
{
    switch(mode)
    {
        case D_GPIO_DEBUG_SWD_JTAG:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
            /* DISABLE会清除SWJ配置字段，恢复完整SWD和JTAG。 */
            GPIO_ConfigPinRemap(GPIO_RMP_SW_JTAG_SW_ENABLE, DISABLE);
            break;

        case D_GPIO_DEBUG_SWD_ONLY:
            RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
            GPIO_ConfigPinRemap(GPIO_RMP_SW_JTAG_SW_ENABLE, ENABLE);
            break;

        default:
            break;
    }
}

void d_gpio_init(d_gpio_port_t port, d_gpio_pin_t pin, d_gpio_mode_t mode, d_gpio_pull_t pull)
{
    GPIO_InitType  gpio_init;
    GPIO_Module   *module;

    module = d_gpio_get_module(port);
    if((module == 0) || (pin == 0U))
    {
        return;
    }

    d_gpio_enable_clock(port);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = pin;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;

    switch(mode)
    {
        case d_GPIO_MODE_INPUT:
        case d_GPIO_MODE_IT_RISING:
        case d_GPIO_MODE_IT_FALLING:
        case d_GPIO_MODE_IT_RISING_FALLING:
        case d_GPIO_MODE_INPUT_AF0_SPI0:
        case d_GPIO_MODE_INPUT_AF1_SPI0:
        case d_GPIO_MODE_INPUT_AF0_SPI1:
        case d_GPIO_MODE_INPUT_AF0_SPI2:
        case d_GPIO_MODE_INPUT_AF1_SPI2:
        case d_GPIO_MODE_INPUT_AF0_USART0:
        case d_GPIO_MODE_INPUT_AF1_USART0:
        case d_GPIO_MODE_INPUT_AF0_USART1:
        case d_GPIO_MODE_INPUT_AF1_USART1:
        case d_GPIO_MODE_INPUT_AF0_USART2:
        case d_GPIO_MODE_INPUT_AF1_USART2:
        case d_GPIO_MODE_INPUT_AF2_USART2:
        case d_GPIO_MODE_INPUT_USART3:
        case d_GPIO_MODE_INPUT_USART4:
            if(pull == d_GPIO_PULLUP)
            {
                gpio_init.GPIO_Mode = GPIO_Mode_IPU;
            }
            else if(pull == d_GPIO_PULLDOWN)
            {
                gpio_init.GPIO_Mode = GPIO_Mode_IPD;
            }
            else
            {
                gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
            }
            break;

        case d_GPIO_MODE_OUTPUT_PP:
        case d_GPIO_MODE_OUTPUT_PP_SWJ:
            gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
            break;

        case d_GPIO_MODE_AF_PP_AF0_SPI0:
        case d_GPIO_MODE_AF_PP_AF1_SPI0:
        case d_GPIO_MODE_AF_PP_AF0_SPI1:
        case d_GPIO_MODE_AF_PP_AF0_SPI2:
        case d_GPIO_MODE_AF_PP_AF1_SPI2:
        case d_GPIO_MODE_AF_PP_AF0_USART0:
        case d_GPIO_MODE_AF_PP_AF1_USART0:
        case d_GPIO_MODE_AF_PP_AF0_USART1:
        case d_GPIO_MODE_AF_PP_AF1_USART1:
        case d_GPIO_MODE_AF_PP_AF0_USART2:
        case d_GPIO_MODE_AF_PP_AF2_USART2:
        case d_GPIO_MODE_AF_PP_USART3:
        case d_GPIO_MODE_AF_PP_USART4:
            gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
            break;

        case d_GPIO_MODE_AF_PP_AF1_USART2:
            gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
            gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
            break;

        case d_GPIO_MODE_OUTPUT_OD:
            gpio_init.GPIO_Mode = GPIO_Mode_Out_OD;
            break;

        case d_GPIO_MODE_ANALOG:
        default:
            gpio_init.GPIO_Mode = GPIO_Mode_AIN;
            break;
    }

    GPIO_InitPeripheral(module, &gpio_init);
}

void d_gpio_set_state(d_gpio_port_t port, d_gpio_pin_t pin, uint8_t state)
{
    GPIO_Module *module;

    module = d_gpio_get_module(port);
    if((module == 0) || (pin == 0U))
    {
        return;
    }

    GPIO_WriteBit(module, pin, state ? Bit_SET : Bit_RESET);
}

uint8_t d_gpio_get_state(d_gpio_port_t port, d_gpio_pin_t pin)
{
    GPIO_Module *module;

    module = d_gpio_get_module(port);
    if((module == 0) || (pin == 0U))
    {
        return 0U;
    }

    return GPIO_ReadInputDataBit(module, pin);
}

