#include "boot_crc.h"

/* 软件CRC32，当前用于Boot记录校验，后续也可以用于App镜像完整性校验。 */
uint32_t boot_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL; /* CRC32标准初值。 */
    uint32_t i;                  /* 字节循环下标。 */
    uint8_t bit;                 /* 单字节内部bit循环下标。 */

    if (data == 0)               /* 空指针不能计算。 */
    {
        return 0UL;              /* 返回0表示无效校验输入。 */
    }

    for (i = 0UL; i < len; i++)  /* 逐字节计算CRC。 */
    {
        crc ^= data[i];          /* 当前字节先与CRC低8位异或。 */
        for (bit = 0U; bit < 8U; bit++) /* 每个字节处理8个bit。 */
        {
            if ((crc & 1UL) != 0UL)     /* 最低位为1时需要异或多项式。 */
            {
                crc = (crc >> 1) ^ 0xEDB88320UL; /* 右移并异或CRC32反向多项式。 */
            }
            else
            {
                crc >>= 1;              /* 最低位为0时只右移。 */
            }
        }
    }

    return ~crc;                         /* CRC32标准输出取反。 */
}
