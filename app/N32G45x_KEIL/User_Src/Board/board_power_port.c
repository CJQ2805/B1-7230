#include "Board/board_power_port.h"

#include "App/app_hw_apply.h"
#include "Driver/d_timer.h"

/* 当前板级缓存：保存三路 MOS 的业务开关状态，再统一下发到 AFE/GPIO。 */
static app_bms_control_t s_board_mos_control;

/* 统一下发当前三路 MOS，具体 AFE/GPIO动作由 app_hw_apply_control() 完成。 */
static app_result_t board_power_port_apply(void)
{
    app_hw_apply_control(&s_board_mos_control);
    return APP_RESULT_OK;
}

/* 设置 CMOS：这里只保存业务命令，实际 AFE动作由统一输出函数完成。 */
static app_result_t board_power_port_set_charge_mos(uint8_t enable)
{
    s_board_mos_control.chg_mos_enable = (enable != 0U) ? 1U : 0U;
    return board_power_port_apply();
}

/* 设置 DMOS：这里只保存业务命令，实际 AFE动作由统一输出函数完成。 */
static app_result_t board_power_port_set_discharge_mos(uint8_t enable)
{
    s_board_mos_control.dsg_mos_enable = (enable != 0U) ? 1U : 0U;
    return board_power_port_apply();
}

/* 设置 PDMOS：这里只保存业务命令，实际 GPIO动作由统一输出函数完成。 */
static app_result_t board_power_port_set_precharge_mos(uint8_t enable)
{
    s_board_mos_control.pre_mos_enable = (enable != 0U) ? 1U : 0U;
    return board_power_port_apply();
}

/* 三路 MOS 原子更新：唤醒或状态切换时避免逐路下发造成中间组合状态。 */
static app_result_t board_power_port_set_all_mos(uint8_t cmos_enable,
                                                  uint8_t dmos_enable,
                                                  uint8_t pdmos_enable)
{
    s_board_mos_control.chg_mos_enable = (cmos_enable != 0U) ? 1U : 0U;
    s_board_mos_control.dsg_mos_enable = (dmos_enable != 0U) ? 1U : 0U;
    s_board_mos_control.pre_mos_enable = (pdmos_enable != 0U) ? 1U : 0U;
    return board_power_port_apply();
}

/* 设置 efuse：业务 enable=1 表示二级熔断输出有效，当前 N32 引脚保持高电平有效。 */
static app_result_t board_power_port_set_efuse(uint8_t enable)
{
    app_hw_apply_efuse_set((enable != 0U) ? 1U : 0U);
    return APP_RESULT_OK;
}

/* PDSG 计时使用当前 d_timer 毫秒时基，避免业务模块直接包含 d_timer.h。 */
static uint32_t board_power_port_get_time_ms(void)
{
    return d_timer_get_ms();
}

app_h_power_manage_port_t app_board_power_port_get(void)
{
    app_h_power_manage_port_t port;

    port.set_charge_mos = board_power_port_set_charge_mos;
    port.set_discharge_mos = board_power_port_set_discharge_mos;
    port.set_precharge_mos = board_power_port_set_precharge_mos;
    port.set_all_mos = board_power_port_set_all_mos;
    port.set_efuse = board_power_port_set_efuse;
    port.get_time_ms = board_power_port_get_time_ms;

    return port;
}
