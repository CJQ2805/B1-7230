/*
 * 文件作用：
 *   封装外部FLASH使用的SPI2底层收发，不在这里实现FLASH命令协议。
 *
 * 硬件连接：
 *   PB12=FLASH_SPI_CS，PB13=FLASH_SPI_CLK，PB14=FLASH_SPI_MISO，PB15=FLASH_SPI_MOSI。
 *   SPI2使用默认引脚，片选由GPIO软件控制，SPI模式为Mode 0。
 *
 * 调用约束：
 *   上层先调用select，再完成一条完整FLASH命令，最后调用deselect。
 *   本文件不创建FreeRTOS互斥锁；如果多个任务访问外部FLASH，由上层统一加锁。
 */
#include "Driver/d_flash_spi.h"

#include "Board/board_io.h"
#include "n32g45x_rcc.h"
#include "n32g45x_spi.h"

#define D_FLASH_SPI_TIMEOUT_COUNT ((uint32_t)100000UL)

static bool s_d_flash_spi_ready = false;

static bool d_flash_spi_wait_status(uint16_t status_flag)
{
    uint32_t u32TimeoutCount;

    u32TimeoutCount = D_FLASH_SPI_TIMEOUT_COUNT;
    while (SPI_I2S_GetStatus(SPI2, status_flag) == RESET)
    {
        if (u32TimeoutCount == 0U)
        {
            return false;
        }
        u32TimeoutCount--;
    }

    return true;
}

void d_flash_spi_init(void)
{
    SPI_InitType tSpiInit;

    if (s_d_flash_spi_ready != false)
    {
        return;
    }

    d_gpio_init(BOARD_IO_FLASH_SPI_CLK_PORT, BOARD_IO_FLASH_SPI_CLK_PIN,
                d_GPIO_MODE_AF_PP_AF0_SPI2, d_GPIO_NOPULL);
    d_gpio_init(BOARD_IO_FLASH_SPI_MOSI_PORT, BOARD_IO_FLASH_SPI_MOSI_PIN,
                d_GPIO_MODE_AF_PP_AF0_SPI2, d_GPIO_NOPULL);
    d_gpio_init(BOARD_IO_FLASH_SPI_MISO_PORT, BOARD_IO_FLASH_SPI_MISO_PIN,
                d_GPIO_MODE_INPUT_AF0_SPI2, d_GPIO_NOPULL);
    board_io_flash_spi_cs_set(1U);

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_SPI2, ENABLE);

    SPI_InitStruct(&tSpiInit);
    tSpiInit.DataDirection = SPI_DIR_DOUBLELINE_FULLDUPLEX;
    tSpiInit.SpiMode = SPI_MODE_MASTER;
    tSpiInit.DataLen = SPI_DATA_SIZE_8BITS;
    tSpiInit.CLKPOL = SPI_CLKPOL_LOW;
    tSpiInit.CLKPHA = SPI_CLKPHA_FIRST_EDGE;
    tSpiInit.NSS = SPI_NSS_SOFT;
    tSpiInit.BaudRatePres = SPI_BR_PRESCALER_8;
    tSpiInit.FirstBit = SPI_FB_MSB;
    tSpiInit.CRCPoly = 7U;
    SPI_Init(SPI2, &tSpiInit);
    SPI_Enable(SPI2, ENABLE);

    s_d_flash_spi_ready = true;
}

void d_flash_spi_select(void)
{
    if (s_d_flash_spi_ready == false)
    {
        d_flash_spi_init();
    }
    board_io_flash_spi_cs_set(0U);
}

void d_flash_spi_deselect(void)
{
    board_io_flash_spi_cs_set(1U);
}

bool d_flash_spi_transfer_byte(uint8_t tx_data, uint8_t *rx_data)
{
    if (s_d_flash_spi_ready == false)
    {
        d_flash_spi_init();
    }

    if (s_d_flash_spi_ready == false)
    {
        return false;
    }

    if (d_flash_spi_wait_status(SPI_I2S_TE_FLAG) == false)
    {
        return false;
    }
    SPI_I2S_TransmitData(SPI2, (uint16_t)tx_data);
    if (d_flash_spi_wait_status(SPI_I2S_RNE_FLAG) == false)
    {
        return false;
    }

    if (rx_data != 0)
    {
        *rx_data = (uint8_t)SPI_I2S_ReceiveData(SPI2);
    }
    else
    {
        (void)SPI_I2S_ReceiveData(SPI2);
    }

    return d_flash_spi_wait_status(SPI_I2S_TE_FLAG) &&
           (SPI_I2S_GetStatus(SPI2, SPI_I2S_BUSY_FLAG) == RESET);
}

bool d_flash_spi_transfer(const uint8_t *tx_data, uint8_t *rx_data, uint16_t length)
{
    uint16_t u16ByteIndex;
    uint8_t u8RxData;
    uint8_t u8TxData;

    if (length == 0U)
    {
        return true;
    }
    if (tx_data == 0)
    {
        return false;
    }

    for (u16ByteIndex = 0U; u16ByteIndex < length; u16ByteIndex++)
    {
        u8TxData = tx_data[u16ByteIndex];
        if (d_flash_spi_transfer_byte(u8TxData, &u8RxData) == false)
        {
            return false;
        }
        if (rx_data != 0)
        {
            rx_data[u16ByteIndex] = u8RxData;
        }
    }

    return true;
}
