#ifndef _D_GPIO_H_
#define _D_GPIO_H_

#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_D_GPIO     1
#define Module_Minor_Version_D_GPIO     0

#define GPIO_REMAP_NONE     0

/* 项目层GPIO模式命名，和厂家GPIO_Mode枚举保持隔离。 */
typedef enum
{
    d_GPIO_MODE_INPUT,
    d_GPIO_MODE_OUTPUT_PP,
    d_GPIO_MODE_OUTPUT_OD,
    d_GPIO_MODE_ANALOG,

    d_GPIO_MODE_IT_RISING,
    d_GPIO_MODE_IT_FALLING,
    d_GPIO_MODE_IT_RISING_FALLING,

    d_GPIO_MODE_INPUT_AF0_SPI0,
    d_GPIO_MODE_AF_PP_AF0_SPI0,
    d_GPIO_MODE_INPUT_AF1_SPI0,
    d_GPIO_MODE_AF_PP_AF1_SPI0,
    d_GPIO_MODE_INPUT_AF0_SPI1,
    d_GPIO_MODE_AF_PP_AF0_SPI1,
    d_GPIO_MODE_INPUT_AF0_SPI2,
    d_GPIO_MODE_AF_PP_AF0_SPI2,
    d_GPIO_MODE_INPUT_AF1_SPI2,
    d_GPIO_MODE_AF_PP_AF1_SPI2,
    d_GPIO_MODE_INPUT_AF0_USART0,
    d_GPIO_MODE_AF_PP_AF0_USART0,
    d_GPIO_MODE_INPUT_AF1_USART0,
    d_GPIO_MODE_AF_PP_AF1_USART0,
    d_GPIO_MODE_INPUT_AF0_USART1,
    d_GPIO_MODE_AF_PP_AF0_USART1,
    d_GPIO_MODE_INPUT_AF1_USART1,
    d_GPIO_MODE_AF_PP_AF1_USART1,
    d_GPIO_MODE_INPUT_AF0_USART2,
    d_GPIO_MODE_AF_PP_AF0_USART2,
    d_GPIO_MODE_INPUT_AF1_USART2,
    d_GPIO_MODE_AF_PP_AF1_USART2,
    d_GPIO_MODE_INPUT_AF2_USART2,
    d_GPIO_MODE_AF_PP_AF2_USART2,
    d_GPIO_MODE_INPUT_USART3,
    d_GPIO_MODE_AF_PP_USART3,
    d_GPIO_MODE_INPUT_USART4,
    d_GPIO_MODE_AF_PP_USART4,
    d_GPIO_MODE_OUTPUT_PP_SWJ
} d_gpio_mode_t;

/* d_gpio_init()使用的上下拉选择。 */
typedef enum
{
    d_GPIO_NOPULL = 0,
    d_GPIO_PULLDOWN,
    d_GPIO_PULLUP
} d_gpio_pull_t;

/* 项目层GPIO端口编号，供板级引脚定义使用。 */
typedef enum
{
    D_GPIO_PORT_A = 0,
    D_GPIO_PORT_B,
    D_GPIO_PORT_C,
    D_GPIO_PORT_D,
    D_GPIO_PORT_E,
    D_GPIO_PORT_INVALID
} d_gpio_port_t;

typedef uint16_t d_gpio_pin_t;

#define D_GPIO_PIN(index) ((d_gpio_pin_t)(1UL << (index)))

/* 简单IO描述符：一个GPIO端口加一个引脚掩码。 */
typedef struct
{
    d_gpio_port_t port;
    d_gpio_pin_t  pin;
} d_io_t;

/* 完整IO描述符，供需要切换输入/输出配置的驱动使用。 */
typedef struct
{
    d_gpio_port_t   port;
    d_gpio_pin_t    pin;
    d_gpio_mode_t   mode;
    d_gpio_pull_t   pull;
} d_io_conf_t;

typedef enum
{
    D_GPIO_DEBUG_SWD_JTAG = 0,
    D_GPIO_DEBUG_SWD_ONLY
} d_gpio_debug_mode_t;

/* 按项目层模式和上下拉配置初始化一个引脚。 */
void d_gpio_init(d_gpio_port_t port, d_gpio_pin_t pin, d_gpio_mode_t mode, d_gpio_pull_t pull);
/* 写输出电平；无效空IO会被忽略。 */
void d_gpio_set_state(d_gpio_port_t port, d_gpio_pin_t pin, uint8_t state);
/* 读取输入电平；无效空IO返回0。 */
uint8_t d_gpio_get_state(d_gpio_port_t port, d_gpio_pin_t pin);
/* 配置调试端口，SWD_ONLY保留SWD并释放JTAG引脚。 */
void d_gpio_set_debug_mode(d_gpio_debug_mode_t mode);

#endif
