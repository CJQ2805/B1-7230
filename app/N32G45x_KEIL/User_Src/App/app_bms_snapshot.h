#ifndef __APP_BMS_SNAPSHOT_H__
#define __APP_BMS_SNAPSHOT_H__

#include "App/app_bms_types.h"

/*
 * 统一业务快照接口说明：
 *   1. 初始化：app_bms_snapshot_init() 在系统启动时调用一次，普通字段清零，温度字段设为无效哨兵；
 *      在首次 update 之前，app_bms_snapshot_read() 读取的就是这份缺省快照。
 *   2. 更新：app_bms_snapshot_update() 只负责把当前项目的 AFE、MCU NTC、板级 IO、
 *      功率管理状态适配成 app_bms_snapshot_t，不在这里写故障/MOS/通信策略。
 *   3. 读取：业务模块统一通过 app_bms_snapshot_read() 获取一致性的本地副本；
 *      不允许直接暴露内部对象或在副本外修改快照。
 *   4. 调用周期：DataHandle_Task 在底层采样/状态刷新后每10ms更新一次；
 *      其他业务任务只读取最近一次完整快照。
 *   5. 数据源迁移：切换 AFE、温度来源、板级信号或 MOS 状态来源时，只修改
 *      app_bms_snapshot.c 内部的静态适配函数和 include 依赖，业务模块继续读同一份契约。
 */

/* 初始化统一业务快照，清空所有缓存字段并建立默认只读快照。 */
void app_bms_snapshot_init(void);

/* 从当前项目的数据源刷新一次快照；唯一任务级调用者是DataHandle_Task。 */
void app_bms_snapshot_update(void);

/* 复制读取最近一次完整快照；不返回内部指针，避免跨任务读到半更新数据。 */
void app_bms_snapshot_read(app_bms_snapshot_t *snapshot);

/* 在SOC算法完成一个周期后，把当前算法状态镜像到统一快照。 */
void app_bms_snapshot_update_soc(void);

/* 状态机计算完成后，将统一BMS工作状态写入快照。 */
void app_bms_snapshot_set_work_status(uint8_t bms_work_status);

#endif
