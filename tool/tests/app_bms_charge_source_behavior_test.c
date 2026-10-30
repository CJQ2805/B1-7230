#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test the real fault-update branches with only the RTOS clock replaced. */
#include "App/app_bms_fault.c"

static uint32_t s_test_now_ms;
static app_bms_snapshot_t s_test_snapshot;

static void unexpected_dependency(const char *name)
{
    fprintf(stderr, "Unexpected external call: %s\n", name);
    abort();
}

uint32_t d_timer_get_ms(void) { return s_test_now_ms; }
void app_input_signals_load_detect_rearm(void) { unexpected_dependency("app_input_signals_load_detect_rearm"); }
int SEGGER_RTT_printf(unsigned buffer_index, const char *format, ...)
{ (void)buffer_index; (void)format; unexpected_dependency("SEGGER_RTT_printf"); return 0; }

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot)
{
    *snapshot = s_test_snapshot;
}

void app_hw_apply_load_detect_enable(uint8_t enable_level)
{
    (void)enable_level;
    unexpected_dependency("app_hw_apply_load_detect_enable");
}

uint8_t app_hw_apply_load_detect_level(void)
{
    unexpected_dependency("app_hw_apply_load_detect_level");
    return 0U;
}

void app_hw_apply_clear_afe_dsg_oc_short_fault(void)
{
    unexpected_dependency("app_hw_apply_clear_afe_dsg_oc_short_fault");
}

uint8_t app_hw_apply_clear_afe_fault(uint8_t clear_mask)
{
    (void)clear_mask;
    unexpected_dependency("app_hw_apply_clear_afe_fault");
    return 0U;
}

void app_hw_apply_efuse_set(uint8_t enable)
{
    (void)enable;
    unexpected_dependency("app_hw_apply_efuse_set");
}

bool app_log_request_snapshot(log_trigger_t trigger)
{
    (void)trigger;
    unexpected_dependency("app_log_request_snapshot");
    return false;
}

bool d_flash_acquire(uint32_t timeout_ms)
{
    (void)timeout_ms;
    unexpected_dependency("d_flash_acquire");
    return false;
}

void d_flash_release(void)
{
    unexpected_dependency("d_flash_release");
}

bool d_flash_erase_page(uint32_t page_addr)
{
    (void)page_addr;
    unexpected_dependency("d_flash_erase_page");
    return false;
}

bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)addr;
    (void)data;
    (void)len;
    unexpected_dependency("d_flash_write");
    return false;
}

en_result_t d_timer_delay_check(bool judgement, u32TimDly_t *delay, uint32_t time_count)
{
    if (!judgement)
    {
        *delay = 0U;
        return Error;
    }
    if (*delay == 0U)
    {
        *delay = s_test_now_ms;
        return Error;
    }
    return ((s_test_now_ms - *delay) >= time_count) ? Ok : Error;
}

static void sample_temperature(uint32_t now_ms, bool detc_present,
                               uint8_t bms_work_status,
                               int16_t current_10ma, int16_t max_temp_c)
{
    bms_fault_data_t data;
    memset(&data, 0, sizeof(data));
    s_test_now_ms = now_ms;
    data.detc_present = detc_present;
    data.bms_work_status = bms_work_status;
    data.current_10ma = current_10ma;
    data.charging = (current_10ma > 0);
    data.discharging = (current_10ma < 0);
    data.temp_data_ready = true;
    data.max_cell_temp_c = max_temp_c;
    data.min_cell_temp_c = 25;
    bms_fault_update_temp_faults(&data);
}

static void sample_current(uint32_t now_ms, bool detc_present,
                           bool detd_present, uint8_t bms_work_status,
                           int16_t current_10ma)
{
    bms_fault_data_t data;
    memset(&data, 0, sizeof(data));
    s_test_now_ms = now_ms;
    data.detc_present = detc_present;
    data.detd_present = detd_present;
    data.bms_work_status = bms_work_status;
    data.current_10ma = current_10ma;
    data.charging = (current_10ma > 0);
    data.discharging = (current_10ma < 0);
    bms_fault_update_current_faults(&data);
}

#define CHECK(condition, message) \
    do { if (!(condition)) { fprintf(stderr, "FAIL: %s\n", (message)); return 1; } } while (0)

int main(void)
{
    memset(&gt_app_bms_fault, 0, sizeof(gt_app_bms_fault));

    /* Charger insertion must protect before any charge current flows. */
    sample_temperature(1U, false, APP_BMS_STATUS_STATIC, 0, 25);
    sample_temperature(2U, true, APP_BMS_STATUS_CHARGE, 0, 60);
    sample_temperature(6002U, true, APP_BMS_STATUS_CHARGE, 0, 60);
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc == 1U,
          "DETC at zero current must start normal charge overtemperature warning");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc == 1U,
          "DETC at zero current must start normal charge overtemperature protection");

    memset(&gt_app_bms_fault, 0, sizeof(gt_app_bms_fault));
    sample_temperature(7000U, false, APP_BMS_STATUS_STATIC, 0, 25);
    sample_temperature(8000U, false, APP_BMS_STATUS_FEEDBACK, 100, 60);
    sample_temperature(12000U, false, APP_BMS_STATUS_FEEDBACK, 100, 60);
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc == 0U,
          "positive regenerative current without DETC must not set normal charge overtemperature warning");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc == 0U,
          "positive regenerative current without DETC must not set normal charge overtemperature protection");

    sample_current(15000U, false, true, APP_BMS_STATUS_DISCHARGE, 0);
    sample_current(16000U, false, true, APP_BMS_STATUS_FEEDBACK, 3200);
    sample_current(20000U, false, true, APP_BMS_STATUS_FEEDBACK, 3200);
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ == 0U,
          "regenerative current without DETC must not set normal charge overcurrent warning");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 == 0U,
          "regenerative current without DETC must not set normal charge overcurrent level 1");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 == 0U,
          "regenerative current without DETC must not set normal charge overcurrent level 2");

    sample_current(48000U, false, true, APP_BMS_STATUS_DISCHARGE, 0);
    sample_current(49000U, false, false, APP_BMS_STATUS_CHARGE, 3200);
    sample_current(80000U, false, false, APP_BMS_STATUS_CHARGE, 3200);
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ == 1U,
          "BMS charge status without DETC must set normal charge overcurrent warning");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 == 1U,
          "BMS charge status without DETC must set normal charge overcurrent level 1");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 == 1U,
          "BMS charge status without DETC must set normal charge overcurrent level 2");

    memset(&gt_app_bms_fault, 0, sizeof(gt_app_bms_fault));
    sample_current(81000U, false, true, APP_BMS_STATUS_DISCHARGE, 0);
    sample_current(82000U, false, true, APP_BMS_STATUS_FEEDBACK, 4600);
    sample_current(86000U, false, true, APP_BMS_STATUS_FEEDBACK, 4600);
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc == 1U,
          "DETD present and DETC absent must retain regenerative overcurrent warning");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc == 1U,
          "DETD present and DETC absent must retain regenerative overcurrent protection");
    CHECK(gt_app_bms_fault.tbms_sw_fault_flag.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 == 0U,
          "regenerative current must not also set normal charge overcurrent level 1");

    memset(&gt_app_bms_fault, 0, sizeof(gt_app_bms_fault));
    memset(&s_test_snapshot, 0, sizeof(s_test_snapshot));
    s_test_snapshot.soc.u8soc_fcc_flag = 1U;
    app_bms_fault_update_afe();
    bms_fault_update_status();
    CHECK(gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag == 1U,
          "FCC must set the full-charge protection bit");
    CHECK(gt_app_bms_fault.tbms_fault_stat.u8charge_pro_flag == 1U,
          "FCC must forbid charge MOS through charge protection");
    CHECK(gt_app_bms_fault.tbms_fault_stat.u8discharge_pro_flag == 0U,
          "FCC must not forbid discharge MOS");

    s_test_snapshot.soc.u8soc_fcc_flag = 0U;
    app_bms_fault_update_afe();
    bms_fault_update_status();
    CHECK(gt_app_bms_fault.tbms_fault_flag.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag == 0U,
          "full-charge protection must clear when FCC clears");
    CHECK(gt_app_bms_fault.tbms_fault_stat.u8charge_pro_flag == 0U,
          "FCC charge-protection request must clear when FCC clears");

    puts("PASS: charge faults and FCC protection");
    return 0;
}
