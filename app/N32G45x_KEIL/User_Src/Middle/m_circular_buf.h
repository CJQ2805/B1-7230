#ifndef _M_CIRCULAR_BUF_H_
#define _M_CIRCULAR_BUF_H_

#include <stdint.h>


#define M_CIRCULAR_BUF_MODE_COMMON   1
#define M_CIRCULAR_BUF_MODE_RING     2

/* 通用环形缓存；预留一个空槽用于区分满和空。 */
typedef struct m_circular_buf
{
    char acName[20];            /* 调试名称。 */
    volatile uint16_t u16WritePos;   /* 下一次写入位置。 */
    volatile uint16_t u16ReadPos;    /* 下一次读取位置。 */
    volatile uint8_t* pu8RxBuf;      /* 实际存储区。 */
    volatile uint16_t u16RxBufSize;  /* 实际存储区大小。 */
    volatile uint8_t u8UseMode;      /* M_CIRCULAR_BUF_MODE_COMMON或M_CIRCULAR_BUF_MODE_RING。 */
} m_circular_buf_t;

/* 状态查询接口。 */
uint16_t m_circular_buf_data_len(m_circular_buf_t * ptCircBuf);
int m_circular_buf_is_empty(m_circular_buf_t * ptCircBuf);
int m_circular_buf_is_full(m_circular_buf_t * ptCircBuf);

/* 单字节读写接口；m_circular_buf_put_byte返回-1无效、0满、1成功。 */
void m_circular_buf_put_linear(m_circular_buf_t * ptCircBuf, uint8_t u8Val);
void m_circular_buf_put_byte_drop(m_circular_buf_t * ptCircBuf, uint8_t u8Val);
int m_circular_buf_put_byte(m_circular_buf_t * ptCircBuf, uint8_t u8Val);
int m_circular_buf_get_byte(m_circular_buf_t * ptCircBuf, uint8_t* pu8Val);

/* m_circular_buf_copy通过复制描述符来预览数据，不移动真实读指针。 */
int m_circular_buf_copy(m_circular_buf_t * ptCircBuf, uint8_t* pu8Buf, uint8_t u8CopyLen);
/* 只复位读写位置，不清除缓存内容。 */
void m_circular_buf_reset_position(m_circular_buf_t * ptCircBuf);

/* 批量读取数据，返回实际读到的字节数。 */
uint16_t m_circular_buf_read(m_circular_buf_t * ptCircBuf, uint8_t* pu8Val, uint16_t u16len);

/* 兼容接口：尽量读写指定长度，不阻塞等待数据或空间。 */
int m_circular_buf_read_data(uint8_t* pu8rx_buf, uint16_t u16rx_buf_len, uint16_t u16read_len, m_circular_buf_t * ptCircBuf);
int m_circular_buf_write_data(uint8_t* pu8tx_data, uint16_t u16tx_data_len, m_circular_buf_t * ptCircBuf);
void m_circular_buf_clear(m_circular_buf_t * ptCircBuf);

#endif
