#include <assert.h>
#include <stdint.h>

#include "App/app_bms_cuv_temp_policy.h"

#define CUV1_TEMP_WARM_BOUNDARY_C 10
#define CUV1_TEMP_COLD_BOUNDARY_C 0
#define CUV1_TEMP_HYSTERESIS_C 1

static app_bms_cuv_temp_band_t update_band(
    app_bms_cuv_temp_band_state_t *state,
    int16_t temperature_c,
    uint8_t temperature_valid)
{
    return app_bms_cuv_temp_band_update(state,
                                        temperature_c,
                                        temperature_valid,
                                        CUV1_TEMP_WARM_BOUNDARY_C,
                                        CUV1_TEMP_COLD_BOUNDARY_C,
                                        CUV1_TEMP_HYSTERESIS_C);
}

static void test_warm_mid_boundary_has_one_degree_hysteresis(void)
{
    app_bms_cuv_temp_band_state_t state = {0U, APP_BMS_CUV_TEMP_BAND_WARM};

    assert(update_band(&state, 10, 1U) == APP_BMS_CUV_TEMP_BAND_WARM);
    assert(update_band(&state, 9, 1U) == APP_BMS_CUV_TEMP_BAND_MID);
    assert(update_band(&state, 10, 1U) == APP_BMS_CUV_TEMP_BAND_MID);
    assert(update_band(&state, 11, 1U) == APP_BMS_CUV_TEMP_BAND_WARM);
}

static void test_mid_cold_boundary_has_one_degree_hysteresis(void)
{
    app_bms_cuv_temp_band_state_t state = {0U, APP_BMS_CUV_TEMP_BAND_MID};

    assert(update_band(&state, 0, 1U) == APP_BMS_CUV_TEMP_BAND_MID);
    assert(update_band(&state, -1, 1U) == APP_BMS_CUV_TEMP_BAND_COLD);
    assert(update_band(&state, 0, 1U) == APP_BMS_CUV_TEMP_BAND_COLD);
    assert(update_band(&state, 1, 1U) == APP_BMS_CUV_TEMP_BAND_MID);
}

static void test_invalid_temperature_uses_warm_profile_without_losing_band(void)
{
    app_bms_cuv_temp_band_state_t state = {0U, APP_BMS_CUV_TEMP_BAND_COLD};

    assert(update_band(&state, -5, 1U) == APP_BMS_CUV_TEMP_BAND_COLD);
    assert(update_band(&state, 0, 0U) == APP_BMS_CUV_TEMP_BAND_WARM);
    assert(state.band == APP_BMS_CUV_TEMP_BAND_COLD);
    assert(update_band(&state, 0, 1U) == APP_BMS_CUV_TEMP_BAND_COLD);
    assert(update_band(&state, 1, 1U) == APP_BMS_CUV_TEMP_BAND_MID);
}

int main(void)
{
    test_warm_mid_boundary_has_one_degree_hysteresis();
    test_mid_cold_boundary_has_one_degree_hysteresis();
    test_invalid_temperature_uses_warm_profile_without_losing_band();
    return 0;
}
