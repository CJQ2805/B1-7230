#ifndef _M_SOFT_I2C_H_
#define _M_SOFT_I2C_H_

#include "Driver/d_gpio.h"
#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_M_Soft_I2C    1
#define Module_Minor_Version_M_Soft_I2C    0

/* 软件IIC引脚及其输入/输出电气配置。 */
typedef struct
{
    d_io_t clk;
    d_io_t sda;
    d_io_conf_t input_conf;
    d_io_conf_t output_conf;
} m_soft_i2c_bus_t;

/* IIC实例封装；兼容旧接口中mutex为true表示总线可用。 */
typedef struct m_soft_i2c_t
{
    m_soft_i2c_bus_t bus;
    bool mutex;
} m_soft_i2c_t;

/* 初始化软件IIC引脚并释放总线。 */
void m_soft_i2c_init(m_soft_i2c_t *i2c);
/* 将IIC引脚切到模拟模式，并标记总线不可用。 */
void m_soft_i2c_deinit(m_soft_i2c_t *i2c);
/* 向一个寄存器地址写入数据；MemAddSize仅为兼容旧接口保留。 */
bool m_soft_i2c_write(m_soft_i2c_t *i2c, uint16_t DevAddress, uint16_t MemAddress,
                 uint16_t MemAddSize, uint8_t *data, uint16_t length);
/* 从一个寄存器地址读取数据；MemAddSize仅为兼容旧接口保留。 */
bool m_soft_i2c_read(m_soft_i2c_t *i2c, uint16_t DevAddress, uint16_t MemAddress,
                uint16_t MemAddSize, uint8_t *buffer, uint16_t length);

/* 移植AFE代码使用的旧锁接口。 */
bool m_soft_i2c_lock_status(m_soft_i2c_t *i2c);
void m_soft_i2c_lock(m_soft_i2c_t *i2c);
void m_soft_i2c_unlock(m_soft_i2c_t *i2c);

#endif
