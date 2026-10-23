#ifndef __SD_TIME_H__
#define __SD_TIME_H__

#include <stdint.h>

#include "sd_result.h"

/*
 * 用户实现位置：Board/Driver 适配层。
 * App 模块禁止直接依赖厂家时基接口，只能通过该回调读取毫秒时间。
 */
typedef app_result_t (*sd_time_get_ms_fn)(void *user_ctx, uint32_t *now_ms);

/*
 * 毫秒时间端口，统一单位为毫秒。
 * 用户实现位置：由用户在 Board/Driver 适配层填充 user_ctx 和 get_ms。
 */
typedef struct
{
    void *user_ctx;              /* 用户自定义上下文，供底层实现保存实例或句柄。 */
    sd_time_get_ms_fn get_ms;    /* 毫秒读取入口，成功时把当前时间写入 now_ms。 */
} sd_time_port_t;

#endif
