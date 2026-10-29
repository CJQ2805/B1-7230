#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "app_soc.h"

void SOC_FCCHandle(long current, unsigned int max_mv, unsigned char uv,
                   unsigned char ov, unsigned int min_mv);
void app_sop_task(int8_t max_temp, int8_t min_temp, uint8_t soc,
                  int32_t current, uint16_t max_voltage)
{
    (void)max_temp; (void)min_temp; (void)soc; (void)current; (void)max_voltage;
}

int main(void)
{
    unsigned tick;
    long previous_remaining = 29840L;
    memset(&gt_app_soc, 0, sizeof(gt_app_soc));
    gt_app_soc.s32battery_full_cap = 30000L;
    gt_app_soc.s32remain_cap = previous_remaining;
    gt_app_soc.tsoc_capacity_data.s32display_cap_val = 30000L;
    gt_app_soc.tsoc_capacity_data.s32real_cap_val = 30000L;
    gt_app_soc.u8display_soc = 99U;

    for (tick = 1U; tick <= 24U; ++tick)
    {
        gt_app_soc.u32tim_250ms_cnt = tick;
        SOC_FCCHandle(5800L, 4114U, 0U, 0U, 4100U);
        assert(gt_app_soc.s32remain_cap >= previous_remaining);
        assert(gt_app_soc.s32remain_cap <= 29850L);
        assert(gt_app_soc.u8display_soc == 99U);
        assert(gt_app_soc.u8soc_fcc_flag == 0U);
        previous_remaining = gt_app_soc.s32remain_cap;
    }
    assert(gt_app_soc.s32remain_cap == 29850L);

    /* The existing full-charge condition is still allowed to reach 100%. */
    for (tick = 25U; tick <= 44U; ++tick)
    {
        gt_app_soc.u32tim_250ms_cnt = tick;
        SOC_FCCHandle(400L, 4114U, 0U, 0U, 4100U);
        if (tick < 44U)
        {
            assert(gt_app_soc.s32remain_cap == 29850L);
            assert(gt_app_soc.u8display_soc == 99U);
        }
    }
    assert(gt_app_soc.u8soc_fcc_flag == 1U);
    assert(gt_app_soc.s32remain_cap == 30000L);
    assert(gt_app_soc.u8display_soc == 100U);
    return 0;
}
