#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Exercise the production queue/commit path with simulated Flash and snapshots. */
#include "App/app_log_manage.c"

static app_bms_snapshot_t sample;
static d_system_reset_flags_t boot_flags;
static log_rtd_t saved[64];
static unsigned saved_count;
static unsigned saved_prints;
static unsigned failed_prints;
static int fail_append;

void app_bms_snapshot_read(app_bms_snapshot_t *out) { *out = sample; }
void app_bms_fault_snapshot_read(struct _app_bms_fault_t *out) { memset(out, 0, sizeof(*out)); }
uint32_t d_timer_get_ms(void) { return 1234; }
uint32_t d_system_get_core_clock_hz(void) { return 144000000; }
void d_system_get_reset_flags(d_system_reset_flags_t *out) { *out = boot_flags; }
bool d_rtc_read(d_rtc_time_t *out) { (void)out; return false; }
int SEGGER_RTT_printf(unsigned channel, const char *format, ...) {
    (void)channel;
    if (strstr(format, "LOG_SAVED")) { assert(saved_count > saved_prints); saved_prints++; }
    if (strstr(format, "LOG_WRITE_FAILED")) failed_prints++;
    return 0;
}
m_flash_wl_status_t m_flash_wl_init(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg) {
    (void)area; (void)cfg; return M_FLASH_WL_OK;
}
m_flash_wl_status_t m_flash_wl_append(m_flash_wl_area_t *area, const uint8_t *data, uint16_t len) {
    (void)area;
    assert(len == 70 && saved_count < 64);
    if (fail_append) return M_FLASH_WL_ERR_FLASH;
    memcpy(&saved[saved_count], data, len);
    assert(saved[saved_count].checksum == app_log_calc_checksum(&saved[saved_count]));
    saved_count++;
    return M_FLASH_WL_OK;
}
m_flash_wl_status_t m_flash_wl_get_info(const m_flash_wl_area_t *area, m_flash_wl_info_t *info) {
    (void)area; memset(info, 0, sizeof(*info)); info->valid_record_count = saved_count;
    return M_FLASH_WL_OK;
}
m_flash_wl_status_t m_flash_wl_read_by_index(const m_flash_wl_area_t *area, uint16_t index,
    uint8_t *buffer, uint16_t capacity, uint16_t *length, uint32_t *seq) {
    (void)area; (void)seq;
    if (!index || index > saved_count || capacity < 70) return M_FLASH_WL_ERR_PARAM;
    memcpy(buffer, &saved[index - 1], 70); *length = 70; return M_FLASH_WL_OK;
}
static void boot(int watchdog) {
    memset(&sample, 0, sizeof(sample));
    memset(&boot_flags, 0, sizeof(boot_flags));
    boot_flags.por_reset = (uint8_t)(watchdog ? 0U : 1U);
    boot_flags.iwdg_reset = (uint8_t)watchdog;
    saved_count = saved_prints = failed_prints = 0;
    fail_append = 0;
    app_log_capture_reset_flags();
    app_log_init();
}
static void test_mos_sequence_does_not_save(void) {
    boot(0);
    app_log_task();
    sample.mos.chg_mos_state = 1; app_log_task();
    sample.mos.pre_mos_state = 1; app_log_task();
    sample.mos.dsg_mos_state = 1; app_log_task();
    sample.mos.pre_mos_state = 0; app_log_task();
    assert(!app_log_request_snapshot(LOG_TRIGGER_SIGNAL_CHANGE));
    app_log_task();
    assert(saved_count == 0 && app_log_get_flush_status() == APP_LOG_FLUSH_COMPLETE);
    assert(app_log_request_snapshot(LOG_TRIGGER_FAULT_SET));
    app_log_task();
    assert(saved_count == 1 && saved[0].ex_sig.bit.chg_mos_s && saved[0].ex_sig.bit.dsg_mos_s);
}
static void test_reset_once_despite_queue_overflow(void) {
    unsigned i;
    boot(1);
    for (i = 0; i < 25; ++i) assert(app_log_request_snapshot(LOG_TRIGGER_FAULT_SET));
    for (i = 0; i < 25; ++i) app_log_task();
    assert(saved_count == 20 && saved_prints == 20);
    assert(saved[0].deg_data[0] == LOG_TRIGGER_RESET);
    assert(saved[0].reset_s.bit.iwd_rst == 1);
    assert(saved[0].hw_err.status == 0U && saved[0].sw_warn.status == 0U && saved[0].sw_err.status == 0U);
    for (i = 1; i < saved_count; ++i) assert(saved[i].reset_s.reset_s == 0);
}
static void test_failed_commit_does_not_consume_reset_or_report_saved(void) {
    boot(1);
    assert(app_log_request_snapshot(LOG_TRIGGER_FAULT_CLEAR));
    fail_append = 1; app_log_task();
    assert(saved_count == 0 && saved_prints == 0 && failed_prints == 1);
    assert(s_log_reset_flags_pending == 1 && app_log_get_flush_status() == APP_LOG_FLUSH_FAILED);
    /* Simulate storage recovery; the failed reset entry must retain its source. */
    fail_append = 0; s_log_ready = 1; app_log_task();
    assert(saved_count == 1 && saved[0].reset_s.bit.sft_rst && saved_prints == 1);
}
static void test_watchdog_and_shutdown_still_save(void) {
    boot(1); app_log_task();
    assert(saved_count == 1 && saved[0].deg_data[0] == LOG_TRIGGER_RESET);
    assert(saved[0].reset_s.bit.iwd_rst);
    assert(app_log_request_snapshot(LOG_TRIGGER_SOFT_SHUTDOWN)); app_log_task();
    assert(saved_count == 2 && saved[1].sw_err.bit.power_off_f);
    assert(saved[1].reset_s.reset_s == 0);
}
int main(void) {
    assert(sizeof(log_rtd_t) == 70);
    test_mos_sequence_does_not_save();
    test_reset_once_despite_queue_overflow();
    test_failed_commit_does_not_consume_reset_or_report_saved();
    test_watchdog_and_shutdown_still_save();
    puts("PASS: MOS sequence, reset once, overflow, failed commit, watchdog and shutdown");
    return 0;
}
