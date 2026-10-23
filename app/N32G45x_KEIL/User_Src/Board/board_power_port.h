#ifndef __BOARD_POWER_PORT_H__
#define __BOARD_POWER_PORT_H__

#include "App/app_h_power_manage.h"

/*
 * 文件作用：
 *   当前 N32 工程的高压/MOS 板级适配端口，把 App 层的 MOS、efuse 和毫秒时基
 *   注入接口连接到现有 app_hw_apply、AFE、GPIO 和 d_timer 路径。
 *
 * 主要用途：
 *   1. 统一承接充电 MOS、放电 MOS、预放 MOS 三个动作。
 *   2. 承接二级熔断输出和 PDSG 计时用毫秒时基。
 *   3. 保持 app_h_power_manage_port_t 中的 enable 语义始终表示“业务上是否使能”，
 *      具体 MOS 高低电平、AFE 命令和 efuse 引脚电平只在本适配层解释。
 *   4. 为 app_h_power_manage 内部初始化提供当前工程端口。
 *
 * 调用方法：
 *   1. app_h_power_manage_init() 内部调用 app_board_power_port_get() 获取端口。
 *   2. Board 层只负责硬件动作适配；snapshot、故障汇总和 MOS 判断由上层组合后传入。
 *
 * 用户修改位置：
 *   1. 如果充电 MOS、放电 MOS 不再由当前 AFE 控制，请修改 board_power_port.c 中
 *      MOS 编号到 app_hw_apply 或 AFE 动作的映射关系。
 *   2. 如果预放 MOS 的 GPIO 连接位置变化，请先修改 Board 侧端口定义；
 *      对外端口语义仍保持 enable 业务值不变。
 *   3. 如果新项目的 MOS 有效电平与当前工程不同，只能在 Board、Driver、AFE 适配层
 *      处理取反，不能在 App 模块里反向解释 enable 业务值。
 *   4. 如果 efuse 输出极性或时基来源变化，也只修改本 Board 适配实现。
 */

/* 获取当前 N32 工程的 MOS 适配端口。 */
app_h_power_manage_port_t app_board_power_port_get(void);

#endif
