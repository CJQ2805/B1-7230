#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../app/N32G45x_KEIL/User_Src/SOX/app_soc.h"

uint8_t app_soc_can_write_matches_table(uint8_t requested_soc,
                                        uint16_t max_cell_voltage_mv,
                                        const uint16_t *cell_voltage_table);
uint8_t app_soc_set_requested_percent(uint8_t requested_soc);

int main(void)
{
    uint16_t cell_voltage_table[101];
    uint16_t index;

    for (index = 0U; index <= 100U; index++)
    {
        cell_voltage_table[index] = (uint16_t)(3000U + (index * 10U));
    }

    assert(app_soc_can_write_matches_table(50U, 3450U, cell_voltage_table) != 0U);
    assert(app_soc_can_write_matches_table(50U, 3550U, cell_voltage_table) != 0U);
    assert(app_soc_can_write_matches_table(50U, 3440U, cell_voltage_table) == 0U);
    assert(app_soc_can_write_matches_table(50U, 3560U, cell_voltage_table) == 0U);
    assert(app_soc_can_write_matches_table(101U, 3500U, cell_voltage_table) == 0U);
    assert(app_soc_can_write_matches_table(50U, 1999U, cell_voltage_table) == 0U);
    assert(app_soc_can_write_matches_table(50U, 3500U, 0) == 0U);

    memset(&gt_app_soc, 0, sizeof(gt_app_soc));
    gt_app_soc.s32battery_full_cap = 1001L;
    assert(app_soc_set_requested_percent(37U) != 0U);
    assert(gt_app_soc.u8display_soc == 37U);
    assert(gt_app_soc.s32remain_cap == 371L);
    assert(gt_app_soc.tsoc_capacity_data.s32display_cap_val == 371L);
    assert(gt_app_soc.tsoc_capacity_data.s32real_cap_val == 371L);
    assert(gt_app_soc.tsoc_capacity_data.s32cap_cnt == 0L);

    assert(app_soc_set_requested_percent(100U) != 0U);
    assert(gt_app_soc.u8display_soc == 100U);
    assert(gt_app_soc.s32remain_cap == 1001L);
    assert(app_soc_set_requested_percent(0U) != 0U);
    assert(gt_app_soc.u8display_soc == 0U);
    assert(gt_app_soc.s32remain_cap == 0L);

    assert(app_soc_set_requested_percent(101U) == 0U);
    assert(gt_app_soc.u8display_soc == 0U);
    assert(gt_app_soc.s32remain_cap == 0L);
    return 0;
}
