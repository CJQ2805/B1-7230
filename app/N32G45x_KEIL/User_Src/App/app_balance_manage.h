#ifndef __APP_BALANCE_MANAGE_H__
#define __APP_BALANCE_MANAGE_H__

#include <stdint.h>

#include "App/app_param.h"

/*
 * 文件作用：
 *   APP层AFE均衡策略模块，对外只暴露周期任务和调试读取接口。
 *
 * 主要用途：
 *   1. 根据统一业务快照判断是否允许开启AFE单体均衡。
 *   2. 生成DVC1124需要的单体均衡位图，再通过HSI_AFE接口写入AFE。
 *   3. 提供当前均衡状态，供状态诊断观察；均衡状态不阻止系统进入STOP。
 *
 * 适用范围：
 *   适用于当前DVC1124项目。阈值、最低允许电压、启停压差统一由app_param.h配置。
 *
 * 调用关系：
 *   AFE_Task以10ms周期调用 app_balance_task()；
 *   低功耗诊断通过 app_balance_is_active() 记录均衡状态，休眠门禁不判断该状态。
 */

/*
 * AFE均衡周期任务。
 * 由AFE_Task以10ms周期执行；模块内部只在均衡状态或位图变化时写AFE寄存器。
 */
void app_balance_task(void);

/* 返回当前软件策略是否认为均衡正在开启，1为开启，0为关闭。 */
uint8_t app_balance_is_active(void);

/* 返回当前请求AFE开启的均衡位图，bit0对应第1串，bit19对应第20串。 */
uint32_t app_balance_get_mask(void);

#endif
