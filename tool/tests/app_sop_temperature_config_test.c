/* Run against the real SOP implementation with default and changed temperature boundaries. */
#include <stdio.h>
#include "../../app/N32G45x_KEIL/User_Src/App/app_param.h"

#ifdef SOP_TEST_OVERRIDE
#undef APP_SOP_DISCHARGE_TEMP_MIN_C
#undef APP_SOP_DISCHARGE_TEMP_ROW1_C
#undef APP_SOP_DISCHARGE_TEMP_ROW2_C
#undef APP_SOP_DISCHARGE_TEMP_ROW3_C
#undef APP_SOP_DISCHARGE_TEMP_ROW4_C
#undef APP_SOP_DISCHARGE_TEMP_ROW5_C
#undef APP_SOP_DISCHARGE_TEMP_ROW6_C
#undef APP_SOP_DISCHARGE_TEMP_ROW7_C
#undef APP_SOP_DISCHARGE_TEMP_ROW8_C
#undef APP_SOP_DISCHARGE_TEMP_ROW9_C
#undef APP_SOP_CHARGE_TEMP_MIN_C
#undef APP_SOP_CHARGE_TEMP_ROW1_C
#undef APP_SOP_CHARGE_TEMP_ROW2_C
#undef APP_SOP_CHARGE_TEMP_ROW3_C
#undef APP_SOP_CHARGE_TEMP_ROW4_C
#undef APP_SOP_CHARGE_TEMP_ROW5_C
#undef APP_SOP_CHARGE_TEMP_ROW6_C
#undef APP_SOP_CHARGE_TEMP_MAX_C
#undef APP_SOP_RECHARGE_TEMP_MIN_C
#undef APP_SOP_RECHARGE_TEMP_ROW1_C
#undef APP_SOP_RECHARGE_TEMP_MAX_C
#undef APP_SOP_HIGH_TEMP_C
#define APP_SOP_DISCHARGE_TEMP_MIN_C (-30)
#define APP_SOP_DISCHARGE_TEMP_ROW1_C (-15)
#define APP_SOP_DISCHARGE_TEMP_ROW2_C (-5)
#define APP_SOP_DISCHARGE_TEMP_ROW3_C 5
#define APP_SOP_DISCHARGE_TEMP_ROW4_C 15
#define APP_SOP_DISCHARGE_TEMP_ROW5_C 25
#define APP_SOP_DISCHARGE_TEMP_ROW6_C 35
#define APP_SOP_DISCHARGE_TEMP_ROW7_C 45
#define APP_SOP_DISCHARGE_TEMP_ROW8_C 52
#define APP_SOP_DISCHARGE_TEMP_ROW9_C 57
#define APP_SOP_CHARGE_TEMP_MIN_C (-12)
#define APP_SOP_CHARGE_TEMP_ROW1_C (-2)
#define APP_SOP_CHARGE_TEMP_ROW2_C 8
#define APP_SOP_CHARGE_TEMP_ROW3_C 18
#define APP_SOP_CHARGE_TEMP_ROW4_C 28
#define APP_SOP_CHARGE_TEMP_ROW5_C 38
#define APP_SOP_CHARGE_TEMP_ROW6_C 48
#define APP_SOP_CHARGE_TEMP_MAX_C 53
#define APP_SOP_RECHARGE_TEMP_MIN_C (-12)
#define APP_SOP_RECHARGE_TEMP_ROW1_C (-2)
#define APP_SOP_RECHARGE_TEMP_MAX_C 62
#define APP_SOP_HIGH_TEMP_C 35
#endif

#ifdef SOP_TEST_INVALID
#undef APP_SOP_CHARGE_TEMP_ROW1_C
#define APP_SOP_CHARGE_TEMP_ROW1_C APP_SOP_CHARGE_TEMP_ROW2_C
#endif

#include "../../app/N32G45x_KEIL/User_Src/SOX/app_sop.c"

struct _app_soc_t gt_app_soc;
unsigned char app_soc_delay(unsigned char judge, unsigned long *base,
                            unsigned long delay, unsigned char set, unsigned char target)
{
    (void)judge; (void)base; (void)delay; (void)set; (void)target;
    return 0;
}

static unsigned failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s\n", __LINE__, #expr); ++failures; } } while (0)

int main(void)
{
#ifdef SOP_TEST_OVERRIDE
    static const int discharge_edges[] = {-15, -5, 5, 15, 25, 35, 45, 52, 57};
    static const int charge_edges[] = {-2, 8, 18, 28, 38, 48};
    const int discharge_min = -30, charge_min = -12, charge_max = 53;
    const int recharge_min = -12, recharge_boundary = -2, recharge_max = 62, high_switch = 35;
#else
    static const int discharge_edges[] = {-10, 0, 10, 20, 30, 40, 50, 55, 58};
    static const int charge_edges[] = {0, 10, 20, 30, 40, 50};
    const int discharge_min = -25, charge_min = -10, charge_max = 55;
    const int recharge_min = -10, recharge_boundary = 0, recharge_max = 60, high_switch = 40;
#endif
    unsigned i;

    CHECK(GetDischargeSOPLevel(0, 50, (int8_t)(discharge_min - 1)) == 0);
    CHECK(GetDischargeSOPLevel(0, 50, (int8_t)discharge_min) > 0);
    for (i = 0; i < sizeof(discharge_edges) / sizeof(discharge_edges[0]); ++i)
    {
        SOP_Get_DischargeTempSOC_Level(50, (int8_t)(discharge_edges[i] - 1));
        CHECK(gt_app_sop.tbms_discharge_level.u8temp_level == i);
        SOP_Get_DischargeTempSOC_Level(50, (int8_t)discharge_edges[i]);
        CHECK(gt_app_sop.tbms_discharge_level.u8temp_level == i + 1U);
    }

    CHECK(GetChargeSOPLevel(50, (int8_t)(charge_min - 1)) == 0);
    CHECK(GetChargeSOPLevel(50, (int8_t)charge_min) > 0);
    CHECK(GetChargeSOPLevel(50, (int8_t)(charge_max - 1)) > 0);
    CHECK(GetChargeSOPLevel(50, (int8_t)charge_max) == 0);
    for (i = 0; i < sizeof(charge_edges) / sizeof(charge_edges[0]); ++i)
    {
        SOP_Get_ChargeTempSOC_Level(50, (int8_t)(charge_edges[i] - 1));
        CHECK(gt_app_sop.tbms_charge_level.u8temp_level == i);
        SOP_Get_ChargeTempSOC_Level(50, (int8_t)charge_edges[i]);
        CHECK(gt_app_sop.tbms_charge_level.u8temp_level == i + 1U);
    }

    SOP_GetRechargeSOPCurrent(95, 10, (int8_t)(recharge_min - 1));
    CHECK(gt_app_sop.u16max_recharge_current01A == 0);
    SOP_GetRechargeSOPCurrent(95, 10, (int8_t)recharge_min);
    CHECK(gt_app_sop.u16max_recharge_current01A == 30);
    SOP_GetRechargeSOPCurrent(95, 10, (int8_t)(recharge_boundary - 1));
    CHECK(gt_app_sop.u16max_recharge_current01A == 30);
    SOP_GetRechargeSOPCurrent(95, 10, (int8_t)recharge_boundary);
    CHECK(gt_app_sop.u16max_recharge_current01A == 300);
    SOP_GetRechargeSOPCurrent(96, 10, (int8_t)recharge_boundary);
    CHECK(gt_app_sop.u16max_recharge_current01A == 0);
    SOP_GetRechargeSOPCurrent(100, 10, (int8_t)recharge_boundary);
    CHECK(gt_app_sop.u16max_recharge_current01A == 0);
    SOP_GetRechargeSOPCurrent(95, (int8_t)(recharge_max - 1), 0);
    CHECK(gt_app_sop.u16max_recharge_current01A == 300);
    SOP_GetRechargeSOPCurrent(95, (int8_t)recharge_max, 0);
    CHECK(gt_app_sop.u16max_recharge_current01A == 0);

    app_sop_task((int8_t)high_switch, -10, 50, 0);
    CHECK(gt_app_sop.tbms_discharge_level.u8temp_level == 1U);
    app_sop_task((int8_t)(high_switch + 1), -10, 50, 0);
    CHECK(gt_app_sop.tbms_discharge_level.u8temp_level == 6U);

    printf("SOP temperature boundaries: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
