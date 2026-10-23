#ifndef __APP_CELL_TOPOLOGY_H__
#define __APP_CELL_TOPOLOGY_H__

#include "app_param.h"

#define APP_BMS_TOPOLOGY_INVALID_INDEX 0xFFU

/* 将业务逻辑串号映射到AFE物理采样通道；逻辑末节对应板级接线的物理顶节。 */
static inline uint8_t app_bms_topology_logical_to_afe_index(uint8_t u8LogicalIndex,
                                                             uint8_t u8LogicalCount)
{
    if ((u8LogicalCount == 0U) ||
        (u8LogicalCount > APP_BMS_MAX_CELL_NUM) ||
        (u8LogicalIndex >= u8LogicalCount))
    {
        return APP_BMS_TOPOLOGY_INVALID_INDEX;
    }

    if (u8LogicalIndex == (uint8_t)(u8LogicalCount - 1U))
    {
        return APP_BMS_AFE_TOP_CELL_INDEX;
    }

    if (u8LogicalIndex >= APP_BMS_AFE_SCAN_CELL_COUNT)
    {
        return APP_BMS_TOPOLOGY_INVALID_INDEX;
    }

    return u8LogicalIndex;
}

/* 读取指定逻辑串对应的AFE物理通道电压；无效串号或映射返回0。 */
static inline uint16_t app_bms_topology_get_cell_voltage_mv(const uint16_t *pu16AfeVoltageMv,
                                                              uint8_t u8LogicalIndex,
                                                              uint8_t u8LogicalCount)
{
    uint8_t u8AfeIndex;

    if (pu16AfeVoltageMv == 0)
    {
        return 0U;
    }

    u8AfeIndex = app_bms_topology_logical_to_afe_index(u8LogicalIndex,
                                                        u8LogicalCount);
    if ((u8AfeIndex == APP_BMS_TOPOLOGY_INVALID_INDEX) ||
        (u8AfeIndex >= APP_BMS_AFE_SCAN_CELL_COUNT))
    {
        return 0U;
    }

    return pu16AfeVoltageMv[u8AfeIndex];
}

/* 将业务均衡位图映射为AFE物理通道位图，避免末节电压和均衡通道错位。 */
static inline uint32_t app_bms_topology_balance_to_afe(uint32_t u32LogicalMask,
                                                       uint8_t u8LogicalCount)
{
    uint32_t u32AfeMask = 0UL;
    uint8_t u8LogicalIndex;

    if ((u8LogicalCount == 0U) || (u8LogicalCount > APP_BMS_MAX_CELL_NUM))
    {
        return 0UL;
    }

    for (u8LogicalIndex = 0U; u8LogicalIndex < u8LogicalCount; u8LogicalIndex++)
    {
        uint8_t u8AfeIndex;

        if ((u32LogicalMask & (1UL << u8LogicalIndex)) == 0UL)
        {
            continue;
        }

        u8AfeIndex = app_bms_topology_logical_to_afe_index(u8LogicalIndex,
                                                            u8LogicalCount);
        if ((u8AfeIndex != APP_BMS_TOPOLOGY_INVALID_INDEX) && (u8AfeIndex < 32U))
        {
            u32AfeMask |= (1UL << u8AfeIndex);
        }
    }

    return u32AfeMask;
}

#endif /* __APP_CELL_TOPOLOGY_H__ */
