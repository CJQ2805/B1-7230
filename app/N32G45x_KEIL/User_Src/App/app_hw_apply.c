/*
 * 文件作用：
 *   硬件动作适配层，把业务模块输出的 app_bms_control_t 命令转换成当前项目的
 *   AFE、MOS、板级IO和系统动作。
 *
 * 主要用途：
 *   1. 隔离业务模块和具体硬件动作。
 *   2. 让故障、MOS、低功耗等模块以后只输出命令，不直接操作 HSI_afe 或 Board。
 *   3. 为更换芯片或迁移模块时减少改动范围。
 *
 * 适用范围：
 *   适用于当前 N32G45X + DVC1124 项目的硬件动作落地。其他项目复用业务模块时，
 *   只需要重新实现本文件中的动作适配接口。
 *
 * 调用关系：
 *   后续业务模块输出 app_bms_control_t 后调用 app_hw_apply_control()；
 *   Task 2 新增的 board_power_port 也通过该入口复用当前N32工程的实际硬件路径；
 *   低功耗流程可调用 app_hw_apply_prepare_sleep() 和 app_hw_apply_restore_after_wakeup()。
 *
 * 注意事项：
 *   第一阶段只提供统一入口，不强行接管现有 MOS 管理逻辑，避免改变当前可运行行为。
 */
#include "App/app_hw_apply.h"
#include "App/app_input_signals.h"
#include "Board/board_io.h"
#include "HSI_afe/hsi_afe.h"
#include "Driver/d_gpio.h"

void app_hw_apply_control(const app_bms_control_t *control)
{
    uint8_t afe_clear_mask = APP_HW_AFE_CLEAR_NONE;

    if (control == 0)
    {
        return;
    }

    /* control 中的三个字段都是业务 enable 语义，具体高低电平和AFE命令映射由适配层内部负责。 */
    hsi_afe_setmos(&gt_hsi_afe,
                   control->chg_mos_enable ? MOS_ON : MOS_OFF,
                   control->dsg_mos_enable ? MOS_ON : MOS_OFF);

    d_gpio_set_state(BOARD_IO_MCU_PDSG_PORT,
                     BOARD_IO_MCU_PDSG_PIN,
                     control->pre_mos_enable ? 1U : 0U);

    if (control->clear_afe_oc_fault != 0U)
    {
        afe_clear_mask |= (uint8_t)(APP_HW_AFE_CLEAR_OCC1 |
                                    APP_HW_AFE_CLEAR_OCC2 |
                                    APP_HW_AFE_CLEAR_OCD1 |
                                    APP_HW_AFE_CLEAR_OCD2);
    }

    if (control->clear_afe_sc_fault != 0U)
    {
        afe_clear_mask |= APP_HW_AFE_CLEAR_SCD;
    }

    if (afe_clear_mask != APP_HW_AFE_CLEAR_NONE)
    {
        (void)app_hw_apply_clear_afe_fault(afe_clear_mask);
    }
}

void app_hw_apply_prepare_sleep(void)
{
    /* 低功耗硬件动作暂时仍由现有低功耗模块执行，后续逐步迁移到这里。 */
}

void app_hw_apply_restore_after_wakeup(void)
{
    /* 唤醒后的硬件恢复暂时仍由现有低功耗模块执行，后续逐步迁移到这里。 */
}

void app_hw_apply_dcdc_enable(uint8_t enable)
{
    board_io_dcdc_enable(enable);
}

void app_hw_apply_load_detect_enable(uint8_t enable_level)
{
    board_io_load_detect_enable(enable_level);
}

uint8_t app_hw_apply_load_detect_level(void)
{
    /* PE8关闭时PE9可能被上拉为高，不能把这个高电平当成负载移除。 */
    if (board_io_load_detect_enabled() == 0U)
    {
        return APP_HW_LOAD_DETECT_INVALID;
    }

    return (gt_app_input_signals.mcu_det_load == 0U) ? 1U : 0U;
}

uint8_t app_hw_apply_clear_afe_fault(uint8_t clear_mask)
{
    uint8_t afe_mask = AFE_MASK_NA;

    if (clear_mask == APP_HW_AFE_CLEAR_NONE)
    {
        return 0U;
    }

    if ((clear_mask & APP_HW_AFE_CLEAR_SCD) != 0U) { afe_mask |= AFE_MASK_SCD; }
    if ((clear_mask & APP_HW_AFE_CLEAR_OCC2) != 0U) { afe_mask |= AFE_MASK_OCC2; }
    if ((clear_mask & APP_HW_AFE_CLEAR_OCD2) != 0U) { afe_mask |= AFE_MASK_OCD2; }
    if ((clear_mask & APP_HW_AFE_CLEAR_OCC1) != 0U) { afe_mask |= AFE_MASK_OCC1; }
    if ((clear_mask & APP_HW_AFE_CLEAR_OCD1) != 0U) { afe_mask |= AFE_MASK_OCD1; }
    if ((clear_mask & APP_HW_AFE_CLEAR_CUV) != 0U) { afe_mask |= AFE_MASK_CUV; }
    if ((clear_mask & APP_HW_AFE_CLEAR_COV) != 0U) { afe_mask |= AFE_MASK_COV; }
    if ((clear_mask & APP_HW_AFE_CLEAR_IWF) != 0U) { afe_mask |= AFE_MASK_IWF; }

    /* 这里只下发DVC1124清除命令，不修改本地状态缓存，等待下一次状态读取确认结果。 */
    return hsi_afe_clear_error(&gt_hsi_afe, afe_mask) ? 1U : 0U;
}

void app_hw_apply_clear_afe_dsg_oc_short_fault(void)
{
    (void)app_hw_apply_clear_afe_fault((uint8_t)(APP_HW_AFE_CLEAR_OCD1 |
                                                  APP_HW_AFE_CLEAR_OCD2 |
                                                  APP_HW_AFE_CLEAR_SCD));
}

void app_hw_apply_clear_afe_chg_oc_fault(void)
{
    /* 清除一级、二级充电过流位，状态缓存等待下一次AFE读取更新。 */
    (void)app_hw_apply_clear_afe_fault((uint8_t)(APP_HW_AFE_CLEAR_OCC1 | APP_HW_AFE_CLEAR_OCC2));
}

uint8_t app_hw_apply_clear_all_afe_fault(void)
{
    /* DVC1124 R0的全部故障位均通过同一清故障命令处理。 */
    return app_hw_apply_clear_afe_fault((uint8_t)(APP_HW_AFE_CLEAR_SCD |
                                                  APP_HW_AFE_CLEAR_OCC2 |
                                                  APP_HW_AFE_CLEAR_OCD2 |
                                                  APP_HW_AFE_CLEAR_OCC1 |
                                                  APP_HW_AFE_CLEAR_OCD1 |
                                                  APP_HW_AFE_CLEAR_CUV |
                                                  APP_HW_AFE_CLEAR_COV |
                                                  APP_HW_AFE_CLEAR_IWF));
}

void app_hw_apply_efuse_set(uint8_t enable)
{
    d_gpio_set_state(BOARD_IO_MCU_FUSE_2ND_PORT,
                     BOARD_IO_MCU_FUSE_2ND_PIN,
                     enable ? 1U : 0U);
}

