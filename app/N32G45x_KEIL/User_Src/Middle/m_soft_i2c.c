/*
 * 文件作用：
 *   软件IIC中间件，通过抽象GPIO接口模拟IIC时序。
 *
 * 主要用途：
 *   1. 给DVC1124 AFE提供IIC读写能力。
 *   2. 在没有硬件IIC或需要特殊时序时复用。
 *
 * 适用范围：
 *   与具体芯片无关，但需要调用方提供SCL/SDA的抽象IO配置。
 *
 * 调用关系：
 *   hsi_afe_dvc1124.c 调用本模块访问AFE；本模块调用 d_gpio 和 m_delay。
 *
 * 注意事项：
 *   软件IIC是阻塞式位操作，调用频率和单次读写长度需要受任务周期约束。
 */
#include "Middle/m_soft_i2c.h"
#include "Middle/m_delay.h"
#include <stddef.h>

/* 读写状态机步骤。 */
#define Slave_Addr      0
#define Mem_Addr_LSB    2
#define Slave_Addr_WR   3
#define Data            4
#define Stop            5

/* dir=0切输入释放开漏线，dir=1切输出驱动总线。 */
static void m_soft_i2c_set_dir(d_io_t *io, uint8_t dir)
{
    if(dir == 0)
    {
        d_gpio_init(io->port, io->pin, d_GPIO_MODE_INPUT, d_GPIO_NOPULL);
    }
    else
    {
        d_gpio_init(io->port, io->pin, d_GPIO_MODE_OUTPUT_OD, d_GPIO_NOPULL);
    }
}

/* 写一根软件IIC线的电平。 */
static void m_soft_i2c_write_pin(d_io_t *io, uint8_t state)
{
    d_gpio_set_state(io->port, io->pin, state);
}

/* 读取一根软件IIC线的电平。 */
static uint8_t m_soft_i2c_read_pin(d_io_t *io)
{
    return d_gpio_get_state(io->port, io->pin);
}

/* 初始化DVC1124总线，并生成参考代码要求的释放时序。 */
static void m_soft_i2c_init_dvc1124(m_soft_i2c_bus_t *bus)
{
    d_gpio_init(bus->clk.port, bus->clk.pin, d_GPIO_MODE_OUTPUT_OD, d_GPIO_NOPULL);
    d_gpio_init(bus->sda.port, bus->sda.pin, d_GPIO_MODE_OUTPUT_OD, d_GPIO_NOPULL);

    m_soft_i2c_write_pin(&bus->clk, 1);
    m_soft_i2c_write_pin(&bus->sda, 0);
    m_delay_ms(5);
    m_soft_i2c_write_pin(&bus->sda, 1);
}

/* 软件模拟边沿延时，保持较短以免单次AFE服务占用太久。 */
static void m_soft_i2c_delay(void)
{
    m_delay_busy(2);
}

/* IIC起始条件：SCL为高时SDA产生下降沿。 */
static void m_soft_i2c_start(m_soft_i2c_bus_t *bus)
{
    m_soft_i2c_write_pin(&bus->sda, 1);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->clk, 1);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->sda, 0);
    m_soft_i2c_delay();
}

/* IIC停止条件：SCL为高时SDA产生上升沿。 */
static void m_soft_i2c_stop(m_soft_i2c_bus_t *bus)
{
    m_soft_i2c_write_pin(&bus->sda, 0);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->clk, 1);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->sda, 1);
    m_soft_i2c_delay();
}

/* 接收1字节后发送ACK(0)或NACK(1)。 */
static void m_soft_i2c_send_ack(m_soft_i2c_bus_t *bus, uint8_t u8AckLevel)
{
    m_soft_i2c_write_pin(&bus->sda, u8AckLevel);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->clk, 1);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->clk, 0);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->sda, 1);
    m_soft_i2c_delay();
}

/* 等待从机ACK；收到ACK返回0，超时或NACK返回-1。 */
static int8_t m_soft_i2c_rev_ack(m_soft_i2c_bus_t *bus)
{
    int8_t i8AckResult = -1;
    uint8_t u8TimeoutCount = 0U;

    m_soft_i2c_set_dir(&bus->sda, 0);
    m_soft_i2c_write_pin(&bus->clk, 1);
    m_soft_i2c_delay();

    do
    {
        if(u8TimeoutCount > 200U)
        {
            i8AckResult = -1;
            break;
        }
        if(m_soft_i2c_read_pin(&bus->sda) == 0)
        {
            i8AckResult = 0;
            break;
        }
    } while(++u8TimeoutCount);

    m_soft_i2c_write_pin(&bus->clk, 0);
    m_soft_i2c_delay();
    m_soft_i2c_set_dir(&bus->sda, 1);
    m_soft_i2c_write_pin(&bus->sda, 0);

    return i8AckResult;
}

/* 按高位在前发送1字节。 */
static void m_soft_i2c_send_byte(m_soft_i2c_bus_t *bus, uint8_t u8DataByte)
{
    uint8_t u8BitIndex;

    for(u8BitIndex = 0U; u8BitIndex < 8U; u8BitIndex++)
    {
        m_soft_i2c_write_pin(&bus->clk, 0);
        m_soft_i2c_delay();
        m_soft_i2c_write_pin(&bus->sda, (u8DataByte & 0x80U) ? 1U : 0U);
        m_soft_i2c_delay();
        m_soft_i2c_write_pin(&bus->clk, 1);
        m_soft_i2c_delay();
        u8DataByte <<= 1;
    }

    m_soft_i2c_write_pin(&bus->clk, 0);
    m_soft_i2c_delay();
    m_soft_i2c_write_pin(&bus->sda, 1);
    m_soft_i2c_delay();
}

/* 在首字节已同步时钟后，按高位在前接收1字节。 */
static uint8_t m_soft_i2c_rev_byte(m_soft_i2c_bus_t *bus)
{
    uint8_t u8DataByte = 0U;
    uint8_t u8BitIndex;

    m_soft_i2c_write_pin(&bus->clk, 0);
    m_soft_i2c_delay();
    m_soft_i2c_set_dir(&bus->sda, 0);

    for(u8BitIndex = 0U; u8BitIndex < 8U; u8BitIndex++)
    {
        m_soft_i2c_write_pin(&bus->clk, 1);
        u8DataByte = (uint8_t)((u8DataByte << 1) | m_soft_i2c_read_pin(&bus->sda));
        m_soft_i2c_delay();
        m_soft_i2c_write_pin(&bus->clk, 0);
        m_soft_i2c_delay();
        m_soft_i2c_delay();
    }

    m_soft_i2c_set_dir(&bus->sda, 1);
    return u8DataByte;
}

/* 接收读取流程的首字节，并等待DVC1124释放时钟拉伸。 */
static int8_t m_soft_i2c_rev_first_byte(m_soft_i2c_bus_t *bus, uint8_t *data)
{
    int8_t i8ReadResult;
    uint8_t u8BitIndex;
    uint16_t u16TimeoutCount;

    m_soft_i2c_set_dir(&bus->sda, 0);
    m_soft_i2c_set_dir(&bus->clk, 0);

    for(u16TimeoutCount = 0U; u16TimeoutCount < 1000U; u16TimeoutCount++)
    {
        if(m_soft_i2c_read_pin(&bus->clk) == 1)
        {
            break;
        }
    }

    if(m_soft_i2c_read_pin(&bus->clk) == 0)
    {
        i8ReadResult = -1;
        m_soft_i2c_set_dir(&bus->clk, 1);
        m_soft_i2c_write_pin(&bus->clk, 0);
    }
    else
    {
        i8ReadResult = 0;
        *data = (uint8_t)((*data << 1) | m_soft_i2c_read_pin(&bus->sda));

        m_soft_i2c_set_dir(&bus->clk, 1);
        m_soft_i2c_write_pin(&bus->clk, 0);
        m_soft_i2c_delay();

        for(u8BitIndex = 0U; u8BitIndex < 7U; u8BitIndex++)
        {
            m_soft_i2c_write_pin(&bus->clk, 1);
            *data = (uint8_t)((*data << 1) | m_soft_i2c_read_pin(&bus->sda));
            m_soft_i2c_delay();
            m_soft_i2c_write_pin(&bus->clk, 0);
            m_soft_i2c_delay();
            m_soft_i2c_delay();
        }
    }

    m_soft_i2c_set_dir(&bus->sda, 1);
    return i8ReadResult;
}

/* DVC1124寄存器读取：先写寄存器地址，再重复起始并连续读数据。 */
static bool m_soft_i2c_read_dvc1124(m_soft_i2c_bus_t *bus, uint16_t DevAddress, uint16_t MemAddress,
                                 uint8_t *buffer, uint16_t length)
{
    uint8_t u8TransferState = Slave_Addr;
    uint8_t u8DataIndex = 0U;
    uint8_t bTransferFinished = 0U;
    int8_t i8TransferResult = -1;

    while(bTransferFinished == 0U)
    {
        switch(u8TransferState)
        {
            case Slave_Addr:
                /* 先发送器件写地址，再发送寄存器地址。 */
                m_soft_i2c_start(bus);
                m_soft_i2c_send_byte(bus, (uint8_t)DevAddress);
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                u8TransferState = (i8TransferResult == -1) ? Stop : Mem_Addr_LSB;
                break;

            case Mem_Addr_LSB:
                /* 当前移植驱动中DVC1124寄存器地址为1字节。 */
                m_soft_i2c_send_byte(bus, (uint8_t)MemAddress);
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                u8TransferState = (i8TransferResult == -1) ? Stop : Slave_Addr_WR;
                break;

            case Slave_Addr_WR:
                /* 重复起始用于把总线方向切换为读。 */
                m_soft_i2c_start(bus);
                m_soft_i2c_send_byte(bus, (uint8_t)(DevAddress | 0x01));
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                if(i8TransferResult == -1)
                {
                    u8TransferState = Stop;
                }
                else
                {
                    buffer[u8DataIndex] = 0U;
                    i8TransferResult = m_soft_i2c_rev_first_byte(bus, &buffer[u8DataIndex++]);
                    if(i8TransferResult == -1)
                    {
                        u8TransferState = Stop;
                    }
                    else if(u8DataIndex >= length)
                    {
                        m_soft_i2c_send_ack(bus, 1);
                        u8TransferState = Stop;
                    }
                    else
                    {
                        m_soft_i2c_send_ack(bus, 0);
                        u8TransferState = Data;
                    }
                }
                break;

            case Data:
                /* 除最后1字节外都回ACK，最后1字节回NACK后停止。 */
                buffer[u8DataIndex++] = m_soft_i2c_rev_byte(bus);
                if(u8DataIndex >= length)
                {
                    m_soft_i2c_send_ack(bus, 1);
                    u8TransferState = Stop;
                }
                else
                {
                    m_soft_i2c_send_ack(bus, 0);
                }
                break;

            case Stop:
                /* 无论成功或失败都以停止条件结束，确保总线释放。 */
                m_soft_i2c_stop(bus);
                bTransferFinished = 1U;
                break;

            default:
                i8TransferResult = -1;
                u8TransferState = Stop;
                break;
        }
    }

    return (i8TransferResult != -1);
}

/* DVC1124寄存器写入：器件地址、寄存器地址、数据依次发送。 */
static bool m_soft_i2c_write_dvc1124(m_soft_i2c_bus_t *bus, uint16_t DevAddress, uint16_t MemAddress,
                                  uint8_t *data, uint16_t length)
{
    uint8_t u8TransferState = Slave_Addr;
    uint8_t u8DataIndex = 0U;
    uint8_t bTransferFinished = 0U;
    int8_t i8TransferResult = -1;

    while(bTransferFinished == 0U)
    {
        switch(u8TransferState)
        {
            case Slave_Addr:
                /* 开始写事务并等待从机ACK。 */
                m_soft_i2c_start(bus);
                m_soft_i2c_send_byte(bus, (uint8_t)DevAddress);
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                u8TransferState = (i8TransferResult == -1) ? Stop : Mem_Addr_LSB;
                break;

            case Mem_Addr_LSB:
                /* 发送目标寄存器地址。 */
                m_soft_i2c_send_byte(bus, (uint8_t)MemAddress);
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                u8TransferState = (i8TransferResult == -1) ? Stop : Data;
                break;

            case Data:
                /* 连续发送数据，直到全部ACK或出现错误。 */
                m_soft_i2c_send_byte(bus, data[u8DataIndex++]);
                i8TransferResult = m_soft_i2c_rev_ack(bus);
                if(i8TransferResult == -1)
                {
                    u8TransferState = Stop;
                }
                else if(u8DataIndex >= length)
                {
                    i8TransferResult = 0;
                    u8TransferState = Stop;
                }
                break;

            case Stop:
                /* 成功或失败后释放总线。 */
                m_soft_i2c_stop(bus);
                bTransferFinished = 1U;
                break;

            default:
                i8TransferResult = -1;
                u8TransferState = Stop;
                break;
        }
    }

    return (i8TransferResult != -1);
}

/* 供移植AFE驱动使用的软件IIC初始化接口。 */
void m_soft_i2c_init(m_soft_i2c_t *i2c)
{
    if(i2c == NULL)
    {
        return;
    }

    m_soft_i2c_init_dvc1124(&i2c->bus);
    i2c->mutex = true;
}

/* 反初始化时将引脚切到模拟模式，降低漏电和误动作风险。 */
void m_soft_i2c_deinit(m_soft_i2c_t *i2c)
{
    if(i2c == NULL)
    {
        return;
    }

    i2c->mutex = false;
    d_gpio_init(i2c->bus.clk.port, i2c->bus.clk.pin, d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
    d_gpio_init(i2c->bus.sda.port, i2c->bus.sda.pin, d_GPIO_MODE_ANALOG, d_GPIO_NOPULL);
}

/* 兼容封装：本项目DVC1124使用1字节寄存器地址。 */
bool m_soft_i2c_write(m_soft_i2c_t *i2c, uint16_t DevAddress, uint16_t MemAddress,
                 uint16_t MemAddSize, uint8_t *data, uint16_t length)
{
    bool bWriteSuccessful;

    (void)MemAddSize;
    if((i2c == NULL) || (data == NULL) || (length == 0U) || !i2c->mutex)
    {
        return false;
    }

    i2c->mutex = false;
    bWriteSuccessful = m_soft_i2c_write_dvc1124(&i2c->bus, DevAddress, MemAddress, data, length);
    i2c->mutex = true;

    return bWriteSuccessful;
}

/* 兼容封装：本项目DVC1124使用1字节寄存器地址。 */
bool m_soft_i2c_read(m_soft_i2c_t *i2c, uint16_t DevAddress, uint16_t MemAddress,
                uint16_t MemAddSize, uint8_t *buffer, uint16_t length)
{
    bool bReadSuccessful;

    (void)MemAddSize;
    if((i2c == NULL) || (buffer == NULL) || (length == 0U) || !i2c->mutex)
    {
        return false;
    }

    i2c->mutex = false;
    bReadSuccessful = m_soft_i2c_read_dvc1124(&i2c->bus, DevAddress, MemAddress, buffer, length);
    i2c->mutex = true;

    return bReadSuccessful;
}

/* 返回旧接口使用的总线可用标志。 */
bool m_soft_i2c_lock_status(m_soft_i2c_t *i2c)
{
    if(i2c == NULL)
    {
        return false;
    }

    return i2c->mutex;
}

/* 标记总线忙。 */
void m_soft_i2c_lock(m_soft_i2c_t *i2c)
{
    if(i2c == NULL)
    {
        return;
    }

    i2c->mutex = false;
}

/* 标记总线可用。 */
void m_soft_i2c_unlock(m_soft_i2c_t *i2c)
{
    if(i2c == NULL)
    {
        return;
    }

    i2c->mutex = true;
}
