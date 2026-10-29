#include <assert.h>
#include "test_support.h"
void uds_request_reset_after_tx(void);
void uds_1ms_task(void);
static bool save_ok, tx_idle;
static unsigned save_count, reset_count;
d_can_t can0;
void uds_tp_recv_frame(uint8_t a, uint8_t *b, uint8_t c) { (void)a; (void)b; (void)c; }
bool app_can_iap_is_busy(void) { return false; }
bool d_can_send(d_can_t *a, uint32_t b, int c, uint8_t *d, uint8_t e) { (void)a;(void)b;(void)c;(void)d;(void)e; return true; }
bool d_can_is_tx_idle(d_can_t *a) { (void)a; return tx_idle; }
bool app_bms_soc_save_for_transition(void) { save_count++; return save_ok; }
void d_system_reset(void) { reset_count++; }
uint32_t d_timer_get_ms(void) { return 0; }
void service_init(void) {}
void network_task(void) {}
void service_task(void) {}
int main(void)
{
    uds_request_reset_after_tx();
    uds_1ms_task();
    assert(save_count == 0 && reset_count == 0); /* ACK is still in flight */
    tx_idle = true;
    uds_1ms_task();
    assert(save_count == 1 && reset_count == 0); /* failed save blocks reset */
    save_ok = true;
    uds_1ms_task();
    assert(save_count == 2 && reset_count == 1); /* retry succeeds before reset */
    return 0;
}
