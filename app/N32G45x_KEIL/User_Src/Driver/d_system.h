#ifndef __D_SYSTEM_H__
#define __D_SYSTEM_H__

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    uint32_t raw_flags;   /* RCC原始CTRLSTS，便于RTT诊断保留完整硬件复位原因。 */
    uint8_t bor_reset;   /* 欠压复位。 */
    uint8_t pin_reset;   /* NRST引脚复位。 */
    uint8_t por_reset;   /* 上电/掉电复位。 */
    uint8_t soft_reset;  /* 软件复位。 */
    uint8_t iwdg_reset;  /* 独立看门狗复位。 */
    uint8_t wwdg_reset;  /* 窗口看门狗复位。 */
    uint8_t lpwr_reset;  /* 低功耗复位。 */
} d_system_reset_flags_t;

/*
 * 切换到HSI作为系统时钟源。
 *
 * 使用场景：
 * 进入STOP前调用，先把系统从PLL切到HSI，降低进入STOP前的时钟复杂度。
 *
 * 参数：
 * timeout为等待HSI起振和系统时钟切换完成的轮询超时计数。
 *
 * 返回：
 * true表示已经成功切到HSI，false表示等待超时。
 */
bool d_system_switch_to_hsi(uint32_t timeout);

/*
 * 进入STOP0低功耗。
 *
 * 注意：
 * 该函数只执行真正的STOP进入动作，RTC/EXTI等唤醒源必须由上层提前配置好。
 * CPU会停在WFI指令处，直到唤醒源触发后才继续往下执行。
 */
void d_system_enter_stop0(void);

/*
 * STOP唤醒后恢复系统时钟配置。
 *
 * 当前工程通过SystemInit()恢复到项目默认144MHz时钟。
 */
void d_system_restore_clock(void);
/* 读取当前向量表地址。 */
uint32_t d_system_get_vector_table_addr(void);
/* 触发系统软件复位。 */
void d_system_reset(void);
/* 读取当前主频，单位Hz。 */
uint32_t d_system_get_core_clock_hz(void);
/* 读取并转换复位来源标志。 */
void d_system_get_reset_flags(d_system_reset_flags_t *flags);

#endif
