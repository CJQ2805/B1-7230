/*
 * 文件作用：
 *   N32G45X串口驱动实现，负责调试/通信串口初始化、中断收发和环形缓存管理。
 *
 * 主要用途：
 *   1. 中断接收上位机协议字节并写入环形缓存。
 *   2. 提供非阻塞读取和发送接口给通信、IAP模块。
 *   3. 统计收发计数，便于调试串口数据是否完整。
 *
 * 适用范围：
 *   只适用于当前N32G45X串口和引脚重映射方案。更换MCU时保留 d_uart.h，对本文件重新实现。
 *
 * 调用关系：
 *   main.c 初始化串口；中断入口调用UART处理；ducar_uart_protocol和
 *   ducar_uart_iap_protocol调用收发接口。
 *
 * 注意事项：
 *   串口中断中只搬运字节，不解析协议；协议解析必须留在主循环中处理。
 */
#include "Driver/d_uart.h"
#include "Driver/d_timer.h"

#include "Middle/m_circular_buf.h"

#include "Board/board_io.h"
#include "Driver/d_gpio.h"
#include "misc.h"
#include "n32g45x_gpio.h"
#include "n32g45x_rcc.h"
#include "n32g45x_usart.h"

/* 调试/协议串口接收/发送缓存大小和默认波特率。 */
#define UART_RX_BUFFER_SIZE 128U
#define UART_TX_BUFFER_SIZE 256U
#define UART_BAUDRATE       9600U
#define UART_TX_COMPLETE_WAIT_MS  300UL

/* 接收字节由USART2_IRQHandler写入，由任务级协议解析模块读取。 */
static volatile u8 s_uart_rx_buffer[UART_RX_BUFFER_SIZE];
static volatile u8 s_uart_tx_buffer[UART_TX_BUFFER_SIZE];
static m_circular_buf_t s_uart_rx_circ_buf = {
    "usart2_rx",
    0,
    0,
    s_uart_rx_buffer,
    UART_RX_BUFFER_SIZE,
    M_CIRCULAR_BUF_MODE_RING
};
static m_circular_buf_t s_uart_tx_circ_buf = {
    "usart2_tx",
    0,
    0,
    s_uart_tx_buffer,
    UART_TX_BUFFER_SIZE,
    M_CIRCULAR_BUF_MODE_RING
};
static volatile uint8_t s_uart_tx_enqueue_failed;

/* 上板调试用：这些变量只记录接收链路状态，不参与协议逻辑。 */
volatile uint32_t gu32_uart_rx_irq_count = 0U;
volatile uint32_t gu32_uart_rx_byte_count = 0U;
volatile uint32_t gu32_uart_rx_error_count = 0U;
volatile uint32_t gu32_uart_rx_overflow_count = 0U;
volatile uint32_t gu32_uart_tx_byte_count = 0U;
volatile uint32_t gu32_uart_tx_overflow_count = 0U;
volatile uint32_t gu32_uart_tx_complete_timeout_count = 0U;
volatile uint16_t gu16_uart_last_sts = 0U;
volatile uint8_t gu8_uart_last_rx = 0U;
volatile uint8_t gu8_uart_rx_debug_buf[16] = {0U};
volatile uint8_t gu8_uart_rx_debug_pos = 0U;

static uint16_t d_uart_tx_free_len(void)
{
    uint16_t u16UsedLength = m_circular_buf_data_len(&s_uart_tx_circ_buf);

    if (u16UsedLength >= (UART_TX_BUFFER_SIZE - 1U))
    {
        return 0U;
    }

    return (uint16_t)((UART_TX_BUFFER_SIZE - 1U) - u16UsedLength);
}

static void d_uart_tx_kick(void)
{
    USART_ConfigInt(USART2, USART_INT_TXDE, ENABLE);
}

static void d_uart_record_rx_byte(uint8_t data)
{
    gu8_uart_last_rx = data;
    gu8_uart_rx_debug_buf[gu8_uart_rx_debug_pos] = data;
    gu8_uart_rx_debug_pos = (uint8_t)((gu8_uart_rx_debug_pos + 1U) & 0x0FU);
    gu32_uart_rx_byte_count++;

    if (m_circular_buf_put_byte(&s_uart_rx_circ_buf, data) != 1)
    {
        gu32_uart_rx_overflow_count++;
    }
}

static void d_uart_clear_rx_error_if_needed(void)
{
    uint16_t u16UartStatus;

    u16UartStatus = (uint16_t)USART2->STS;
    if ((u16UartStatus & (USART_FLAG_OREF | USART_FLAG_NEF | USART_FLAG_PEF | USART_FLAG_FEF)) != 0U)
    {
        /*
         * 按手册要求：先读状态寄存器，再读数据寄存器，才能清掉ORE/NE/PE/FE。
         * 注意不要在中断里打断点，否则CPU停住后连续发来的字节会造成OREF。
         */
        gu32_uart_rx_error_count++;
        gu16_uart_last_sts = u16UartStatus;
        (void)USART2->STS;
        (void)USART2->DAT;
    }
}

/* 按原理图配置USART2重映射引脚：PB4为TX，PB5为RX。 */
static void d_uart_gpio_init(void)
{
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO, ENABLE);
    GPIO_ConfigPinRemap(GPIO_RMP_SW_JTAG_SW_ENABLE, ENABLE);
    GPIO_ConfigPinRemap(GPIO_RMP3_USART2, ENABLE);

    d_gpio_init(BOARD_IO_MCU_RX_PORT, BOARD_IO_MCU_RX_PIN,
                d_GPIO_MODE_INPUT_AF1_USART2, d_GPIO_NOPULL);
    d_gpio_init(BOARD_IO_MCU_TX_PORT, BOARD_IO_MCU_TX_PIN,
                d_GPIO_MODE_AF_PP_AF1_USART2, d_GPIO_NOPULL);
}

/* USART2中断用于把接收数据写入环形缓存。 */
static void d_uart_nvic_init(void)
{
    NVIC_InitType nvic_init;

    nvic_init.NVIC_IRQChannel                   = USART2_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0;
    nvic_init.NVIC_IRQChannelSubPriority        = 0;
    nvic_init.NVIC_IRQChannelCmd                = ENABLE;
    NVIC_Init(&nvic_init);
}

/* 初始化GPIO、NVIC、USART2和接收环形缓存。 */
void d_uart_init(void)
{
    USART_InitType usart_init;

    m_circular_buf_clear(&s_uart_rx_circ_buf);
    m_circular_buf_clear(&s_uart_tx_circ_buf);
    gu32_uart_rx_irq_count = 0U;
    gu32_uart_rx_byte_count = 0U;
    gu32_uart_rx_error_count = 0U;
    gu32_uart_rx_overflow_count = 0U;
    gu32_uart_tx_byte_count = 0U;
    gu32_uart_tx_overflow_count = 0U;
    gu32_uart_tx_complete_timeout_count = 0U;
    s_uart_tx_enqueue_failed = 0U;
    gu16_uart_last_sts = 0U;
    gu8_uart_last_rx = 0U;
    gu8_uart_rx_debug_pos = 0U;

    d_uart_gpio_init();
    d_uart_nvic_init();

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART2, ENABLE);

    USART_StructInit(&usart_init);
    usart_init.BaudRate            = UART_BAUDRATE;
    usart_init.WordLength          = USART_WL_8B;
    usart_init.StopBits            = USART_STPB_1;
    usart_init.Parity              = USART_PE_NO;
    usart_init.HardwareFlowControl = USART_HFCTRL_NONE;
    usart_init.Mode                = USART_MODE_RX | USART_MODE_TX;

    USART_Init(USART2, &usart_init);
    USART_ConfigInt(USART2, USART_INT_TXDE, DISABLE);
    USART_ConfigInt(USART2, USART_INT_RXDNE, ENABLE);
    USART_Enable(USART2, ENABLE);
}

/* STOP前关闭USART2；RX稍后由低功耗模块改配为下降沿唤醒输入。 */
void d_uart_prepare_stop(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    USART_ConfigInt(USART2, USART_INT_TXDE, DISABLE);
    USART_ConfigInt(USART2, USART_INT_RXDNE, DISABLE);
    NVIC_DisableIRQ(USART2_IRQn);
    NVIC_ClearPendingIRQ(USART2_IRQn);
    USART_Enable(USART2, DISABLE);
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_USART2, DISABLE);

    d_gpio_init(BOARD_IO_MCU_TX_PORT, BOARD_IO_MCU_TX_PIN,
                d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
    d_gpio_init(BOARD_IO_MCU_RX_PORT, BOARD_IO_MCU_RX_PIN,
                d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
    __set_PRIMASK(primask);
}

/* 发送原始字节缓存；只放入TX环形缓存，真正发送由TXDE中断后台完成。 */
bool d_uart_try_send_buffer(const uint8_t* data, uint32_t length)
{
    uint32_t u32ByteIndex;

    if ((data == 0) || (length == 0UL))
    {
        return false;
    }

    USART_ConfigInt(USART2, USART_INT_TXDE, DISABLE); /* 入队期间暂停TX中断，避免读写指针同时变化。 */

    if (length > d_uart_tx_free_len())
    {
        gu32_uart_tx_overflow_count++;
        s_uart_tx_enqueue_failed = 1U;
        if (m_circular_buf_data_len(&s_uart_tx_circ_buf) != 0U)
        {
            d_uart_tx_kick(); /* 原队列里还有数据时，恢复中断继续发送旧数据。 */
        }
        return false;
    }

    for (u32ByteIndex = 0; u32ByteIndex < length; u32ByteIndex++)
    {
        (void)m_circular_buf_put_byte(&s_uart_tx_circ_buf, data[u32ByteIndex]);
    }

    s_uart_tx_enqueue_failed = 0U;
    d_uart_tx_kick();
    return true;
}

void d_uart_send_buffer(const uint8_t* data, uint32_t length)
{
    (void)d_uart_try_send_buffer(data, length);
}

/* 等待移位寄存器发送完成；使用真实毫秒时限，避免TX异常时长时间忙等。 */
bool d_uart_wait_tx_complete(void)
{
    uint32_t u32StartTimeMs;
    uint8_t bTransmitTimedOut = 0U;

    if (s_uart_tx_enqueue_failed != 0U)
    {
        s_uart_tx_enqueue_failed = 0U;
        gu32_uart_tx_complete_timeout_count++;
        return false;
    }

    d_uart_tx_kick();
    u32StartTimeMs = d_timer_get_ms();

    while ((d_uart_tx_pending() != 0U) &&
           ((uint32_t)(d_timer_get_ms() - u32StartTimeMs) < UART_TX_COMPLETE_WAIT_MS))
    {
    }

    if (d_uart_tx_pending() != 0U)
    {
        bTransmitTimedOut = 1U;
    }

    while ((USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET) &&
           ((uint32_t)(d_timer_get_ms() - u32StartTimeMs) < UART_TX_COMPLETE_WAIT_MS))
    {
    }

    if (USART_GetFlagStatus(USART2, USART_FLAG_TXC) == RESET)
    {
        bTransmitTimedOut = 1U;
    }

    if (bTransmitTimedOut != 0U)
    {
        gu32_uart_tx_complete_timeout_count++;
        return false;
    }

    return true;
}

/* 返回接收中断已写入但尚未读取的字节数。 */
uint16_t d_uart_available(void)
{
    return m_circular_buf_data_len(&s_uart_rx_circ_buf);
}

/* 返回TX环形缓存中还没发送完的字节数。 */
uint16_t d_uart_tx_pending(void)
{
    return m_circular_buf_data_len(&s_uart_tx_circ_buf);
}

uint32_t d_uart_get_rx_count(void)
{
    return gu32_uart_rx_byte_count;
}

uint32_t d_uart_get_tx_count(void)
{
    return gu32_uart_tx_byte_count;
}

/* 从接收环形缓存读取1字节。 */
uint8_t d_uart_read_byte(uint8_t* data)
{
    if (data == 0)
    {
        return 0;
    }

    return (uint8_t)(m_circular_buf_get_byte(&s_uart_rx_circ_buf, data) == 1);
}

/* 清空接收缓存状态，通常用于协议重新同步。 */
void d_uart_clear_rx_buffer(void)
{
    m_circular_buf_clear(&s_uart_rx_circ_buf);
}

/* USART2接收中断：把每个收到的字节压入环形缓存。 */
void USART2_IRQHandler(void)
{
    uint16_t u16UartStatus;

    gu32_uart_rx_irq_count++;
    u16UartStatus = (uint16_t)USART2->STS;
    gu16_uart_last_sts = u16UartStatus;

    /*
     * 进中断后尽量把硬件接收寄存器读空。
     * 用flag而不是只用IntStatus，是为了错误状态叠加时也能把RXDNE释放掉。
     */
    while (USART_GetFlagStatus(USART2, USART_FLAG_RXDNE) != RESET)
    {
        d_uart_record_rx_byte((uint8_t)USART_ReceiveData(USART2));
    }

    if (USART_GetFlagStatus(USART2, USART_FLAG_TXDE) != RESET)
    {
        uint8_t u8DataByte;

        if (m_circular_buf_get_byte(&s_uart_tx_circ_buf, &u8DataByte) == 1)
        {
            USART_SendData(USART2, u8DataByte);
            gu32_uart_tx_byte_count++;
        }
        else
        {
            USART_ConfigInt(USART2, USART_INT_TXDE, DISABLE);
        }
    }

    d_uart_clear_rx_error_if_needed();
}

