#ifndef __APP_INPUT_SIGNALS_H__
#define __APP_INPUT_SIGNALS_H__

#include <stdint.h>

/* APP全部外部输入的最终业务状态；DET字段统一为1=在位，0=不在位。 */
typedef struct
{
    uint8_t mcu_det_load;     /* 负载检测：1=在位，0=不在位。 */
    uint8_t mcu_detd;         /* DETD：1=在位，0=不在位。 */
    uint8_t mcu_detc;         /* DETC：1=在位，0=不在位。 */
    uint8_t mcu_2nd_det;      /* 二级检测：1=有效/在位（GPIO低），0=无效/默认高。 */
    uint8_t mcu_sw_present;   /* 按键活动状态：1=按下或松开后5秒内，0=已释放超过5秒。 */
} app_input_signals_t;

/* 采样模块的实时源；业务模块优先读取总快照中的signal.input。 */
extern volatile app_input_signals_t gt_app_input_signals;

void app_input_signals_init(void);
void app_input_signals_update(void);
/* LD检测电路使能后调用：丢弃关闭期间的缓存，按负载在位重新开始消抖。 */
void app_input_signals_load_detect_rearm(void);

#endif
