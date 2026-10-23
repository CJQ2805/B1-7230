#ifndef TEST_ZS_B1_APP_BMS_SNAPSHOT_H
#define TEST_ZS_B1_APP_BMS_SNAPSHOT_H

#include <stdint.h>
#include "App/app_bms_fault.h"

#define APP_BMS_CELL_COUNT 20U
#define APP_BMS_MAX_TEMP_NUM 3U
#define APP_BMS_TEMP_INVALID_VALUE ((int16_t)0x7FFF)
#define APP_BMS_NTC_VALID_MOS 0x08U

typedef struct
{
    uint8_t mcu_detd;
    uint8_t mcu_detc;
} test_input_signals_t;

typedef struct
{
    test_input_signals_t input;
} test_signal_t;

typedef struct
{
    t_bms_fault_flag detail;
    uint8_t charge_pro;
    uint8_t discharge_pro;
    uint8_t general_fault;
    uint8_t pf_fault;
} test_fault_image_t;
typedef test_fault_image_t app_bms_fault_image_t;

typedef struct
{
    test_fault_image_t active;
} test_fault_state_t;

typedef struct
{
    uint8_t u8display_soc;
    uint8_t u8display_soh;
    int32_t s32battery_full_cap;
    uint32_t u32cycle_index;
} test_soc_state_t;

typedef struct
{
    int16_t s16MaxDischargeCurrent10mA;
    int16_t s16MaxChargeCurrent10mA;
    uint16_t u16MaxRechargeCurrent10mA;
    uint32_t u32MaxChargeVoltageMv;
} test_sop_state_t;

typedef struct
{
    uint8_t chg_mos_state;
    uint8_t dsg_mos_state;
} test_mos_state_t;

typedef struct
{
    int16_t mos_temp_0p1c;
    uint8_t valid_mask;
} test_ntc_state_t;

typedef struct
{
    uint8_t cell_ov;
    uint8_t cell_uv;
} test_afe_fault_t;

typedef struct
{
    uint8_t cell_count;
    uint8_t data_ready;
    uint8_t valid_cells;
    uint16_t cell_mv[APP_BMS_CELL_COUNT];
    uint16_t max_cell_mv;
    uint16_t min_cell_mv;
    uint8_t max_cell_num;
    uint8_t min_cell_num;
    uint32_t bat_mv;
    int32_t current_ma;
    uint8_t charging;
    uint8_t discharging;
    uint8_t bms_work_status;
    uint8_t temp_count;
    int16_t temp_c[APP_BMS_MAX_TEMP_NUM];
    int16_t temp_0p1c[APP_BMS_MAX_TEMP_NUM];
    uint8_t temp_valid_mask;
    int16_t max_temp_c;
    int16_t min_temp_c;
    uint8_t max_temp_num;
    uint8_t min_temp_num;
    uint8_t afe_status_valid;
    test_afe_fault_t afe_fault;
    test_ntc_state_t ntc;
    test_fault_state_t fault;
    test_soc_state_t soc;
    test_mos_state_t mos;
    test_sop_state_t sop;
    test_signal_t signal;
} app_bms_snapshot_t;

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot);

#endif
