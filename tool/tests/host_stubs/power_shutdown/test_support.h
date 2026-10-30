#ifndef POWER_SHUTDOWN_TEST_SUPPORT_H
#define POWER_SHUTDOWN_TEST_SUPPORT_H
#include <stdint.h>
#include <stdbool.h>
typedef enum { APP_RESULT_OK, APP_RESULT_NULL_PTR, APP_RESULT_NOT_INIT } app_result_t;
typedef struct { uint8_t chg_mos_state, dsg_mos_state, pre_mos_state; } test_mos_t;
typedef struct { uint8_t afe_status_valid; test_mos_t mos; } app_bms_snapshot_t;
typedef enum { APP_LOG_FLUSH_COMPLETE, APP_LOG_FLUSH_FAILED, APP_LOG_FLUSH_PENDING } app_log_flush_status_t;
typedef enum { LOG_TRIGGER_SOFT_SHUTDOWN } log_trigger_t;
typedef struct { uint8_t shutdown_requested; } hsi_afe_t;
extern hsi_afe_t gt_hsi_afe;
void app_low_power_set_shutdown_inhibit(uint8_t);
void app_h_power_manage_set_poweroff_override(uint8_t);
bool app_can_iap_is_busy(void);
bool ducar_iap_is_busy(void);
bool uds_port_is_reset_pending(void);
void app_bms_snapshot_read(app_bms_snapshot_t *);
bool app_log_request_snapshot(log_trigger_t);
app_log_flush_status_t app_log_get_flush_status(void);
bool app_bms_soc_flash_save(void);
uint32_t d_timer_get_ms(void);
void d_timer_delay_ms(uint32_t);
bool hsi_afe_shutdown(hsi_afe_t *);
void d_adc_deinit(void);
void app_hw_apply_dcdc_enable(uint8_t);
void d_system_reset(void);
int SEGGER_RTT_printf(unsigned, const char *, ...);
#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)
#endif
