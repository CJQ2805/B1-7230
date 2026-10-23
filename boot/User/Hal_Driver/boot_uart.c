#include "boot_uart.h"

#include "Boot/boot_config.h"
#include "SEGGER_RTT.h"
#include "n32g45x_gpio.h"
#include "n32g45x_rcc.h"
#include "n32g45x_usart.h"

/* Boot调试串口沿用APP设计：USART2重映射到PB4/PB5。 */
#define BOOT_UART_PORT      GPIOB
#define BOOT_UART_TX_PIN    GPIO_PIN_4
#define BOOT_UART_RX_PIN    GPIO_PIN_5

static void boot_uart_gpio_init(void)
{
    GPIO_InitType gpio_init;

    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_GPIOB | RCC_APB2_PERIPH_AFIO, ENABLE);
    GPIO_ConfigPinRemap(GPIO_RMP_SW_JTAG_SW_ENABLE, ENABLE);
    GPIO_ConfigPinRemap(GPIO_RMP3_USART2, ENABLE);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = BOOT_UART_RX_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_InitPeripheral(BOOT_UART_PORT, &gpio_init);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = BOOT_UART_TX_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(BOOT_UART_PORT, &gpio_init);
}

void boot_uart_init(void)
{
    USART_InitType usart_init;

    boot_uart_gpio_init();
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART2, ENABLE);

    USART_StructInit(&usart_init);
    usart_init.BaudRate = BOOT_UART_BAUDRATE;
    usart_init.WordLength = USART_WL_8B;
    usart_init.StopBits = USART_STPB_1;
    usart_init.Parity = USART_PE_NO;
    usart_init.HardwareFlowControl = USART_HFCTRL_NONE;
    usart_init.Mode = USART_MODE_RX | USART_MODE_TX;

    USART_Init(USART2, &usart_init);
    USART_Enable(USART2, ENABLE);

    SEGGER_RTT_Init(); /* 调试日志走RTT，UART保留给升级数据接收。 */
}

void boot_uart_send_byte(uint8_t data)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) == RESET)
    {
    }

    USART_SendData(USART2, data);
}

void boot_uart_send_buffer(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (data == 0)
    {
        return;
    }

    for (i = 0UL; i < len; i++)
    {
        boot_uart_send_byte(data[i]);
    }
}

void boot_uart_wait_tx_complete(void)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET)
    {
    }
}

bool boot_uart_read_byte(uint8_t *data)
{
    if (data == 0)
    {
        return false;
    }

    if (USART_GetFlagStatus(USART2, USART_FLAG_RXDNE) == RESET)
    {
        return false;
    }

    *data = (uint8_t)USART_ReceiveData(USART2);
    return true;
}

