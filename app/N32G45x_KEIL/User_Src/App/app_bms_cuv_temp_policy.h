#ifndef __APP_BMS_CUV_TEMP_POLICY_H__
#define __APP_BMS_CUV_TEMP_POLICY_H__

#include <stdint.h>

typedef enum
{
    APP_BMS_CUV_TEMP_BAND_WARM = 0U,
    APP_BMS_CUV_TEMP_BAND_MID,
    APP_BMS_CUV_TEMP_BAND_COLD
} app_bms_cuv_temp_band_t;

typedef struct
{
    uint8_t initialized;
    uint8_t band;
} app_bms_cuv_temp_band_state_t;

/*
 * 依据最低有效电芯温度选择一级欠压保护温度档。
 * 首次有效采样按配置边界分档；运行中在每个边界两侧应用回差。
 * 温度无效时返回暖档（较严格阈值），但保留上一次有效档位状态。
 */
static inline app_bms_cuv_temp_band_t app_bms_cuv_temp_band_update(
    app_bms_cuv_temp_band_state_t *state,
    int16_t temperature_c,
    uint8_t temperature_valid,
    int16_t warm_boundary_c,
    int16_t cold_boundary_c,
    int16_t hysteresis_c)
{
    app_bms_cuv_temp_band_t band;

    if ((state == 0) || (temperature_valid == 0U))
    {
        return APP_BMS_CUV_TEMP_BAND_WARM;
    }

    if ((state->initialized == 0U) ||
        (state->band > (uint8_t)APP_BMS_CUV_TEMP_BAND_COLD))
    {
        if (temperature_c >= warm_boundary_c)
        {
            band = APP_BMS_CUV_TEMP_BAND_WARM;
        }
        else if (temperature_c >= cold_boundary_c)
        {
            band = APP_BMS_CUV_TEMP_BAND_MID;
        }
        else
        {
            band = APP_BMS_CUV_TEMP_BAND_COLD;
        }

        state->band = (uint8_t)band;
        state->initialized = 1U;
        return band;
    }

    band = (app_bms_cuv_temp_band_t)state->band;
    switch (band)
    {
        case APP_BMS_CUV_TEMP_BAND_WARM:
            if (temperature_c <= (warm_boundary_c - hysteresis_c))
            {
                band = (temperature_c <= (cold_boundary_c - hysteresis_c)) ?
                       APP_BMS_CUV_TEMP_BAND_COLD : APP_BMS_CUV_TEMP_BAND_MID;
            }
            break;

        case APP_BMS_CUV_TEMP_BAND_MID:
            if (temperature_c >= (warm_boundary_c + hysteresis_c))
            {
                band = APP_BMS_CUV_TEMP_BAND_WARM;
            }
            else if (temperature_c <= (cold_boundary_c - hysteresis_c))
            {
                band = APP_BMS_CUV_TEMP_BAND_COLD;
            }
            break;

        case APP_BMS_CUV_TEMP_BAND_COLD:
            if (temperature_c >= (cold_boundary_c + hysteresis_c))
            {
                band = (temperature_c >= (warm_boundary_c + hysteresis_c)) ?
                       APP_BMS_CUV_TEMP_BAND_WARM : APP_BMS_CUV_TEMP_BAND_MID;
            }
            break;

        default:
            band = APP_BMS_CUV_TEMP_BAND_WARM;
            break;
    }

    state->band = (uint8_t)band;
    return band;
}

#endif
