#ifndef UDS_TRANSITION_TEST_SUPPORT_H
#define UDS_TRANSITION_TEST_SUPPORT_H
#include <stdbool.h>
#include <stdint.h>
#define REQUEST_ID 0x7E0U
#define FUNCTIONAL_REQUEST_ID 0x7DFU
#define D_CAN_ID_STANDARD 0
typedef struct { int unused; } d_can_t;
extern d_can_t can0;
void uds_tp_recv_frame(uint8_t, uint8_t *, uint8_t);
bool app_can_iap_is_busy(void);
bool d_can_send(d_can_t *, uint32_t, int, uint8_t *, uint8_t);
bool d_can_is_tx_idle(d_can_t *);
bool app_bms_soc_save_for_transition(void);
void d_system_reset(void);
uint32_t d_timer_get_ms(void);
void service_init(void);
void network_task(void);
void service_task(void);
#endif
