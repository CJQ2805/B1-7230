#include <assert.h>
#include <stdint.h>

#include "../../app/N32G45x_KEIL/User_Src/App/app_cell_topology.h"
#include "../../app/N32G45x_KEIL/User_Src/App/app_bms_types.h"

static void test_snapshot_cell_array_matches_configured_logical_count(void)
{
    const uint32_t u32SnapshotCellCapacity =
        (uint32_t)(sizeof(((app_bms_snapshot_t *)0)->cell_mv) / sizeof(uint16_t));

    assert(u32SnapshotCellCapacity == APP_BMS_CELL_COUNT);
}

static void test_20s_top_cell_maps_to_last_logical_slot(void)
{
    assert(APP_BMS_AFE_SCAN_CELL_COUNT == 24U);
    assert(app_bms_topology_logical_to_afe_index(18U, 20U) == 18U);
    assert(app_bms_topology_logical_to_afe_index(19U, 20U) == 23U);
    assert(app_bms_topology_logical_to_afe_index(20U, 20U) == APP_BMS_TOPOLOGY_INVALID_INDEX);
}

static void test_21s_top_cell_maps_to_index_20(void)
{
    assert(app_bms_topology_logical_to_afe_index(19U, 21U) == 19U);
    assert(app_bms_topology_logical_to_afe_index(20U, 21U) == 23U);
}

static void test_24s_mapping_remains_contiguous(void)
{
    assert(app_bms_topology_logical_to_afe_index(22U, 24U) == 22U);
    assert(app_bms_topology_logical_to_afe_index(23U, 24U) == 23U);
}

static void test_physical_top_cell_voltage_returns_in_logical_last_slot(void)
{
    const uint16_t au16AfeVoltage[24] = {
        3000U, 3001U, 3002U, 3003U, 3004U, 3005U, 3006U, 3007U,
        3008U, 3009U, 3010U, 3011U, 3012U, 3013U, 3014U, 3015U,
        3016U, 3017U, 3018U, 3019U, 0U, 0U, 0U, 3056U
    };

    assert(app_bms_topology_get_cell_voltage_mv(au16AfeVoltage, 19U, 20U) == 3056U);
    assert(app_bms_topology_get_cell_voltage_mv(au16AfeVoltage, 19U, 21U) == 3019U);
    assert(app_bms_topology_get_cell_voltage_mv(au16AfeVoltage, 20U, 21U) == 3056U);
}

static void test_balance_mask_maps_logical_top_cell_to_physical_channel_24(void)
{
    const uint32_t u32Logical20sMask = (1UL << 0U) | (1UL << 18U) | (1UL << 19U);
    const uint32_t u32Expected20sPhysicalMask = (1UL << 0U) | (1UL << 18U) | (1UL << 23U);
    const uint32_t u32Logical21sMask = (1UL << 0U) | (1UL << 19U) | (1UL << 20U);
    const uint32_t u32Expected21sPhysicalMask = (1UL << 0U) | (1UL << 19U) | (1UL << 23U);

    assert(app_bms_topology_balance_to_afe(u32Logical20sMask, 20U) == u32Expected20sPhysicalMask);
    assert(app_bms_topology_balance_to_afe(u32Logical21sMask, 21U) == u32Expected21sPhysicalMask);
    assert(app_bms_topology_balance_to_afe(0x00FFFFFFUL, 24U) == 0x00FFFFFFUL);
}

int main(void)
{
    test_snapshot_cell_array_matches_configured_logical_count();
    test_20s_top_cell_maps_to_last_logical_slot();
    test_21s_top_cell_maps_to_index_20();
    test_24s_mapping_remains_contiguous();
    test_physical_top_cell_voltage_returns_in_logical_last_slot();
    test_balance_mask_maps_logical_top_cell_to_physical_channel_24();
    return 0;
}
