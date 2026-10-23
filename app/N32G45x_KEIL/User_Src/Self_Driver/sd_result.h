#ifndef __SD_RESULT_H__
#define __SD_RESULT_H__

#include <stdint.h>

/* Self_Driver 统一返回值，供 App 与用户实现的底层端口共享。 */
typedef enum
{
    APP_RESULT_OK = 0U,            /* 执行成功。 */
    APP_RESULT_NULL_PTR = 1U,      /* 输入参数为空指针。 */
    APP_RESULT_NOT_INIT = 2U,      /* 端口或底层资源尚未初始化。 */
    APP_RESULT_EXTERNAL_FAIL = 3U  /* 外部接口调用失败。 */
} app_result_t;

#endif
