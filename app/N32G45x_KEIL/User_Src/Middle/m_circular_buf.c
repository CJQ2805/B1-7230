/*
 * 文件作用：
 *   通用环形缓存模块，提供字节流先进先出缓存能力。
 *
 * 主要用途：
 *   1. 给UART中断接收缓存字节。
 *   2. 支持主循环非阻塞读取串口数据。
 *   3. 可复用到其他字节流缓存场景。
 *
 * 适用范围：
 *   与芯片和业务无关的通用中间件。迁移到其他项目时只需要同时带走 .c/.h。
 *
 * 调用关系：
 *   d_uart.c 当前使用本模块管理RX/TX缓存。
 *
 * 注意事项：
 *   本模块不做协议解析，也不主动延时；调用方负责处理满缓存和空缓存情况。
 */
#include "m_circular_buf.h"
#include <string.h>

/* 只复位读写位置，缓存中的数据字节不清除。 */
void m_circular_buf_reset_position(m_circular_buf_t * ptCircBuf)
{
    if (!ptCircBuf)
    {
        return;
    }

    ptCircBuf->u16WritePos = 0;
    ptCircBuf->u16ReadPos  = 0;
}

/* 写指针再前进一步会碰到读指针时，表示缓存已满。 */
int m_circular_buf_is_full(m_circular_buf_t * ptCircBuf)
{
    if ((!ptCircBuf) || (ptCircBuf->u16RxBufSize == 0U))
    {
        return 0;
    }

    return (((ptCircBuf->u16WritePos + 1U) % ptCircBuf->u16RxBufSize) == ptCircBuf->u16ReadPos);
}

/* 读写指针相同表示缓存为空。 */
int m_circular_buf_is_empty(m_circular_buf_t * ptCircBuf)
{
    if (!ptCircBuf)
    {
        return 0;
    }

    return (ptCircBuf->u16WritePos == ptCircBuf->u16ReadPos);
}

/* 线性写入兼容接口，不做环形回绕。 */
void m_circular_buf_put_linear(m_circular_buf_t * ptCircBuf, uint8_t u8Val)
{
    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf))
    {
        return;
    }

    if (ptCircBuf->u16WritePos >= ptCircBuf->u16RxBufSize)
    {
        return;
    }

    ptCircBuf->pu8RxBuf[ptCircBuf->u16WritePos++] = u8Val;
}

/* 环形写入接口，缓存满时直接丢弃本次数据。 */
void m_circular_buf_put_byte_drop(m_circular_buf_t * ptCircBuf, uint8_t u8Val)
{
    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf))
    {
        return;
    }

    if (m_circular_buf_is_full(ptCircBuf))
    {
        return;
    }

    ptCircBuf->pu8RxBuf[ptCircBuf->u16WritePos] = u8Val;
    ptCircBuf->u16WritePos = (ptCircBuf->u16WritePos + 1U) % ptCircBuf->u16RxBufSize;
}

/* 带状态返回的环形写入：-1无效，0满，1成功。 */
int m_circular_buf_put_byte(m_circular_buf_t * ptCircBuf, uint8_t u8Val)
{
    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf))
    {
        return -1;
    }

    if (m_circular_buf_is_full(ptCircBuf))
    {
        return 0;
    }

    ptCircBuf->pu8RxBuf[ptCircBuf->u16WritePos] = u8Val;
    ptCircBuf->u16WritePos = (ptCircBuf->u16WritePos + 1U) % ptCircBuf->u16RxBufSize;

    return 1;
}

/* 从环形缓存弹出1字节：-1无效，0空，1成功。 */
int m_circular_buf_get_byte(m_circular_buf_t * ptCircBuf, uint8_t* pu8Val)
{
    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf) || (!pu8Val))
    {
        return -1;
    }

    if (m_circular_buf_is_empty(ptCircBuf))
    {
        return 0;
    }

    *pu8Val = ptCircBuf->pu8RxBuf[ptCircBuf->u16ReadPos];
    ptCircBuf->u16ReadPos = (ptCircBuf->u16ReadPos + 1U) % ptCircBuf->u16RxBufSize;

    return 1;
}

/* 最多读取u16len字节，并返回实际读取数量。 */
uint16_t m_circular_buf_read(m_circular_buf_t * ptCircBuf, uint8_t* pu8Val, uint16_t u16len)
{
    uint16_t u16ByteIndex;

    if (!pu8Val)
    {
        return 0;
    }

    for (u16ByteIndex = 0U; u16ByteIndex < u16len; u16ByteIndex++)
    {
        if (m_circular_buf_get_byte(ptCircBuf, &pu8Val[u16ByteIndex]) != 1)
        {
            break;
        }
    }

    return u16ByteIndex;
}

/* 返回未读字节数，支持读写指针回绕。 */
uint16_t m_circular_buf_data_len(m_circular_buf_t * ptCircBuf)
{
    uint16_t u16WritePosTmp;

    if ((!ptCircBuf) || (ptCircBuf->u16RxBufSize == 0U))
    {
        return 0;
    }

    u16WritePosTmp = ptCircBuf->u16WritePos;

    if (u16WritePosTmp == ptCircBuf->u16ReadPos)
    {
        return 0;
    }

    if (ptCircBuf->u16ReadPos > u16WritePosTmp)
    {
        return ptCircBuf->u16RxBufSize - ptCircBuf->u16ReadPos + u16WritePosTmp;
    }

    return u16WritePosTmp - ptCircBuf->u16ReadPos;
}

/* 通过临时描述符复制数据，不消耗真实缓存中的数据。 */
int m_circular_buf_copy(m_circular_buf_t * ptCircBuf, uint8_t* pu8Buf, uint8_t u8CopyLen)
{
    m_circular_buf_t tCircBuf;
    uint8_t u8ByteIndex;

    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf) || (!pu8Buf))
    {
        return -1;
    }

    tCircBuf = *ptCircBuf;

    for (u8ByteIndex = 0U; u8ByteIndex < u8CopyLen; u8ByteIndex++)
    {
        if (m_circular_buf_get_byte(&tCircBuf, &pu8Buf[u8ByteIndex]) != 1)
        {
            break;
        }
    }

    return u8ByteIndex;
}

/* 尝试读取指定长度；无新数据时立即返回已读长度，不在缓存层阻塞等待。 */
int m_circular_buf_read_data(uint8_t* pu8rx_buf, uint16_t u16rx_buf_len, uint16_t u16read_len, m_circular_buf_t * ptCircBuf)
{
    uint16_t u16ReadLength = 0U;
    uint16_t u16ReadTotal = 0U;
    uint16_t u16TargetLength = u16read_len;

    if (!pu8rx_buf)
    {
        return 0;
    }

    if (u16TargetLength > u16rx_buf_len)
    {
        u16TargetLength = u16rx_buf_len;
    }

    while (u16ReadTotal != u16TargetLength)
    {
        u16ReadLength = m_circular_buf_read(ptCircBuf, &pu8rx_buf[u16ReadTotal], u16TargetLength - u16ReadTotal);
        u16ReadTotal += u16ReadLength;

        if (!u16ReadLength)
        {
            break;
        }
        else if (u16ReadTotal == u16TargetLength)
        {
            break;
        }
    }

    return u16ReadTotal;
}

/* 尝试写入指定长度；缓存满时立即返回已写长度，不在缓存层阻塞等待。 */
int m_circular_buf_write_data(uint8_t* pu8tx_data, uint16_t u16tx_data_len, m_circular_buf_t * ptCircBuf)
{
    uint16_t u16TxByteIndex = 0U;

    if (!pu8tx_data)
    {
        return 0;
    }

    while (u16TxByteIndex != u16tx_data_len)
    {
        if (m_circular_buf_put_byte(ptCircBuf, pu8tx_data[u16TxByteIndex]) != 1)
        {
            break;
        }
        else
        {
            u16TxByteIndex++;
        }
    }

    return u16TxByteIndex;
}

/* 清空存储区并复位读写位置。 */
void m_circular_buf_clear(m_circular_buf_t * ptCircBuf)
{
    if ((!ptCircBuf) || (!ptCircBuf->pu8RxBuf))
    {
        return;
    }

    memset((void*)ptCircBuf->pu8RxBuf, 0, ptCircBuf->u16RxBufSize);
    ptCircBuf->u16WritePos = 0;
    ptCircBuf->u16ReadPos  = 0;
}
