/*
 * 文件作用：
 *   APP层AFE均衡管理模块，负责把“什么时候均衡、均衡哪些单体、什么时候关闭”封装成一个任务接口。
 *
 * 主要用途：
 *   1. 从 app_bms_snapshot 获取最新单体电压、压差、电流方向和充电器接入状态。
 *   2. 按项目条件控制AFE均衡：压差达到30mV、最低单体高于3.2V、处于充电/上电允许状态时开启。
 *   3. 均衡开启后继续保持均衡状态，直到放电、压差降到10mV以内、数据无效或最低单体低于允许值。
 *
 * 适用范围：
 *   适用于当前N32G45X + DVC1124项目。模块本身不直接访问AFE寄存器，只调用HSI_AFE暴露的均衡接口。
 *
 * 调用关系：
 *   AFE_Task以10ms周期调用 app_balance_task()；
 *   app_balance_task() 调用 hsi_afe_balance() 写入AFE均衡寄存器；
 *   app_balance_is_active() 提供当前均衡状态，供诊断观察；均衡状态不阻止进入STOP。
 *
 * 注意事项：
 *   均衡会消耗被均衡单体能量，所以最低单体电压不满足条件时必须关闭；
 *   均衡过程中如果进入放电状态，也必须立即关闭，避免放电时继续拉低部分单体。
 */
#include "App/app_balance_manage.h"

#include "App/app_bms_snapshot.h"
#include "App/app_cell_topology.h"
#include "HSI_afe/hsi_afe.h"

/* 当前软件策略记录的均衡状态，1表示上一次已成功请求AFE开启均衡。 */
static uint8_t s_app_balance_active = 0U;

/* 当前请求AFE开启的均衡单体位图，bit0对应第1串，bit19对应第20串。 */
static uint32_t s_app_balance_mask = 0UL;

/* 最近一次写AFE均衡寄存器是否成功，便于Keil Watch窗口调试观察。 */
static uint8_t s_app_balance_write_ok = 1U;

/* 将AFE均衡请求写成关闭状态，所有退出路径统一走这里，避免状态记录和AFE寄存器不一致。 */
static void app_balance_turn_off(void)
{
    if ((s_app_balance_active != 0U) || (s_app_balance_mask != 0UL))
    {
        s_app_balance_write_ok = hsi_afe_balance(&gt_hsi_afe, 0UL) ? 1U : 0U;
    }
    else
    {
        s_app_balance_write_ok = 1U;
    }

    if (s_app_balance_write_ok != 0U)
    {
        s_app_balance_active = 0U;
        s_app_balance_mask = 0UL;
    }
}

/* 按当前最低单体和阈值生成需要打开均衡的单体位图。 */
static uint32_t app_balance_build_mask(const app_bms_snapshot_t *ptSnapshot,
                                       uint16_t u16BalanceStartDiffMv,
                                       uint8_t bIncludeEqual)
{
    uint8_t u8CellIndex;
    uint8_t u8CellCount;
    uint32_t u32BalanceMask = 0UL;
    uint16_t u16BalanceEnableMv;
    uint8_t bBalanceThisCell;

    u8CellCount = ptSnapshot->cell_count;
    if (u8CellCount > APP_BMS_CELL_COUNT)
    {
        u8CellCount = APP_BMS_CELL_COUNT;
    }

    u16BalanceEnableMv = (uint16_t)(ptSnapshot->min_cell_mv + u16BalanceStartDiffMv);
    for (u8CellIndex = 0U; u8CellIndex < u8CellCount; u8CellIndex++)
    {
        /*
         * 只对有效单体判断均衡。
         * 开启阶段使用30mV阈值；已均衡阶段使用10mV阈值，让高电压单体逐步回到关闭压差内。
         */
        if (ptSnapshot->cell_mv[u8CellIndex] == 0U)
        {
            continue;
        }

        if (bIncludeEqual != 0U)
        {
            bBalanceThisCell = (ptSnapshot->cell_mv[u8CellIndex] >= u16BalanceEnableMv) ? 1U : 0U;
        }
        else
        {
            bBalanceThisCell = (ptSnapshot->cell_mv[u8CellIndex] > u16BalanceEnableMv) ? 1U : 0U;
        }

        if (bBalanceThisCell != 0U)
        {
            u32BalanceMask |= (1UL << u8CellIndex);
        }
    }

    return u32BalanceMask;
}

void app_balance_task(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint16_t u16SelectedDiffMv;
    uint32_t u32BalanceRequestMask;
    uint32_t u32AfeBalanceMask;

#if (APP_BALANCE_ENABLE == 0U)
    app_balance_turn_off();
    return;
#else
    app_bms_snapshot_read(&tBmsSnapshot);

    /*
     * 数据未准备好时不做均衡。
     * 这里选择关闭均衡，是为了避免采样异常时继续按旧数据消耗单体能量。
     */
    if ((tBmsSnapshot.data_ready == 0U) || (tBmsSnapshot.cell_count == 0U))
    {
        app_balance_turn_off();
        return;
    }

    /*
     * 关闭条件1：进入放电状态时立即关闭均衡。
     * 关闭条件2：压差已经回到10mV以内时关闭均衡。
     * 关闭条件3：最低单体不再高于3.2V时关闭均衡。
     */
    if ((tBmsSnapshot.discharging != 0U) ||
        (tBmsSnapshot.cell_gap_mv <= APP_BALANCE_STOP_DIFF_MV) ||
        (tBmsSnapshot.min_cell_mv <= APP_BALANCE_MIN_CELL_MV))
    {
        app_balance_turn_off();
        return;
    }

    /*
     * 未处于均衡状态时，必须同时满足：
     *   1. 压差达到30mV；
     *   2. 最低单体高于3.2V；
     *   3. 当前不处于放电状态（静置或充电都允许）。
     * 已处于均衡状态时，允许按“balance state”继续执行，直到满足上面的关闭条件。
     */
    if (s_app_balance_active == 0U)
    {
        if (tBmsSnapshot.cell_gap_mv < APP_BALANCE_START_DIFF_MV)
        {
            app_balance_turn_off();
            return;
        }

        u16SelectedDiffMv = APP_BALANCE_START_DIFF_MV;
    }
    else
    {
        u16SelectedDiffMv = APP_BALANCE_STOP_DIFF_MV;
    }

    u32BalanceRequestMask = app_balance_build_mask(&tBmsSnapshot,
                                          u16SelectedDiffMv,
                                          (s_app_balance_active == 0U) ? 1U : 0U);
    if (u32BalanceRequestMask == 0UL)
    {
        app_balance_turn_off();
        return;
    }

    /*
     * 只有AFE写入成功后才更新本模块状态。
     * 如果IIC偶发失败，保留上一轮状态，下一次10ms任务会继续尝试。
     */
    u32AfeBalanceMask = app_bms_topology_balance_to_afe(u32BalanceRequestMask,
                                                        tBmsSnapshot.cell_count);
    s_app_balance_write_ok = hsi_afe_balance(&gt_hsi_afe, u32AfeBalanceMask) ? 1U : 0U;
    if (s_app_balance_write_ok != 0U)
    {
        s_app_balance_active = 1U;
        s_app_balance_mask = u32BalanceRequestMask;
    }
#endif
}

uint8_t app_balance_is_active(void)
{
    return s_app_balance_active;
}

uint32_t app_balance_get_mask(void)
{
    return s_app_balance_mask;
}

/* 复制均衡策略状态和AFE写入结果，便于确认均衡条件与IIC写入是否成功。 */

