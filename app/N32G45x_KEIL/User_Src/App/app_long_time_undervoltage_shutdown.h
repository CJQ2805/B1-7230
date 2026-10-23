#ifndef __APP_LONG_TIME_UNDERVOLTAGE_SHUTDOWN_H__
#define __APP_LONG_TIME_UNDERVOLTAGE_SHUTDOWN_H__

/* 初始化长时间欠压关机计时；计时起点仅保存在RAM，复位后重新开始。 */
void app_long_time_undervoltage_shutdown_init(void);

/* 由BMS周期任务调用；CUV1且无充电连续24小时后请求公共关机流程。 */
void app_long_time_undervoltage_shutdown_task(void);

#endif
