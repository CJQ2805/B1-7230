#include <assert.h>
#include <string.h>
#include "test_support.h"

void app_power_manage_init(void);
app_result_t app_power_manage_request_shutdown(void);
void app_power_manage_task(void);

static uint32_t now_ms;
static unsigned save_attempts, shutdown_attempts, reset_count;
static bool save_succeeds, afe_succeeds = true;
static bool upgrade_busy, uart_upgrade_busy, uds_upgrade_reset_pending;
static uint8_t poweroff_override;
static uint32_t live_soc[4], saved_soc[4];
hsi_afe_t gt_hsi_afe;
void app_low_power_set_shutdown_inhibit(uint8_t v) { (void)v; }
void app_h_power_manage_set_poweroff_override(uint8_t v) { poweroff_override = v; }
bool app_can_iap_is_busy(void) { return upgrade_busy; }
bool ducar_iap_is_busy(void) { return uart_upgrade_busy; }
bool uds_port_is_reset_pending(void) { return uds_upgrade_reset_pending; }
void app_bms_snapshot_read(app_bms_snapshot_t *p) { memset(p, 0, sizeof(*p)); p->afe_status_valid = 1; }
bool app_log_request_snapshot(log_trigger_t t) { (void)t; return true; }
app_log_flush_status_t app_log_get_flush_status(void) { return APP_LOG_FLUSH_COMPLETE; }
bool app_bms_soc_flash_save(void) { save_attempts++; if (!save_succeeds) return false; memcpy(saved_soc, live_soc, sizeof(live_soc)); return true; }
uint32_t d_timer_get_ms(void) { return now_ms; }
void d_timer_delay_ms(uint32_t v) { (void)v; }
bool hsi_afe_shutdown(hsi_afe_t *p) { shutdown_attempts++; if (afe_succeeds) p->shutdown_requested = 1; return afe_succeeds; }
void d_adc_deinit(void) {}
void app_hw_apply_dcdc_enable(uint8_t v) { (void)v; }
void d_system_reset(void) { reset_count++; }
int SEGGER_RTT_printf(unsigned c, const char *f, ...) { (void)c; (void)f; return 0; }

int main(void)
{
    app_power_manage_init();
    assert(app_power_manage_request_shutdown() == APP_RESULT_OK);
    now_ms = 10000;
    upgrade_busy = true;
    app_power_manage_task(); /* an active upgrade defers automatic poweroff */
    assert(poweroff_override == 0 && save_attempts == 0);
    upgrade_busy = false;
    uart_upgrade_busy = true;
    app_power_manage_task();
    assert(poweroff_override == 0 && save_attempts == 0);
    uart_upgrade_busy = false;
    uds_upgrade_reset_pending = true;
    app_power_manage_task();
    assert(poweroff_override == 0 && save_attempts == 0);
    uds_upgrade_reset_pending = false;
    app_power_manage_task(); /* MOS off requested */
    assert(poweroff_override == 1);
    app_power_manage_task(); /* MOS confirmed off */
    live_soc[0] = 47; live_soc[1] = 14099; live_soc[2] = 30000; live_soc[3] = 8765;
    app_power_manage_task(); /* failed SOC save */
    assert(save_attempts == 1 && shutdown_attempts == 0);
    now_ms += 1000;
    save_succeeds = true;
    afe_succeeds = false;
    app_power_manage_task(); /* SOC saved but AFE temporarily unavailable */
    assert(shutdown_attempts == 1 && memcmp(live_soc, saved_soc, sizeof(live_soc)) == 0);
    live_soc[1] = 14098; /* SOC continues changing while AFE command retries */
    afe_succeeds = true;
    now_ms += 1000;
    app_power_manage_task();
    assert(shutdown_attempts == 2 && memcmp(live_soc, saved_soc, sizeof(live_soc)) == 0);
    live_soc[1] = 14097;
    now_ms += 3000;
    save_succeeds = false;
    app_power_manage_task();
    assert(reset_count == 0);
    save_succeeds = true;
    now_ms += 1000;
    app_power_manage_task();
    assert(reset_count == 1 && memcmp(live_soc, saved_soc, sizeof(live_soc)) == 0);
    return 0;
}
