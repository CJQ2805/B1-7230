#ifndef __SD_POWER_H__
#define __SD_POWER_H__

#include <stdint.h>

#include "sd_result.h"

/* 统一描述 App 可请求的 MOS 类型，不绑定具体芯片或寄存器。 */
typedef enum
{
    SD_POWER_MOS_CHG = 0U, /* 充电 MOS。 */
    SD_POWER_MOS_DSG = 1U, /* 放电 MOS。 */
    SD_POWER_MOS_PRE = 2U  /* 预放 MOS。 */
} sd_power_mos_id_t;

/* 统一描述 MOS 目标状态，App 只表达开关请求，不直接改硬件电平。 */
typedef enum
{
    SD_POWER_MOS_OFF = 0U, /* 请求关闭 MOS。 */
    SD_POWER_MOS_ON = 1U   /* 请求打开 MOS。 */
} sd_power_mos_state_t;

/*
 * 用户实现位置：Board/Driver 适配层。
 * App 模块禁止直接修改硬件电平，只能通过该回调请求板级 MOS 动作。
 */
typedef app_result_t (*sd_power_write_mos_fn)(void *user_ctx,
                                              sd_power_mos_id_t mos_id,
                                              sd_power_mos_state_t state);

/*
 * 板级 MOS 端口。
 * 用户实现位置：由用户在 Board/Driver 适配层填充 user_ctx 和 write_mos。
 */
typedef struct
{
    void *user_ctx;                   /* 用户自定义上下文，供底层实现保存实例或句柄。 */
    sd_power_write_mos_fn write_mos;  /* MOS 动作入口，负责把通用请求转换成板级执行。 */
} sd_power_port_t;

#endif
