#ifndef BOOT_UDS_PORT_H
#define BOOT_UDS_PORT_H

#include <stdint.h>

#define FRAME_SIZE                  8U
#define REQUEST_ID                  0x7A0U
#define RESPONSE_ID                 0x7A8U
#define FUNCTIONAL_REQUEST_ID       0x7DFU
#define UDS_RX_MAX                  1024U
#define UDS_TX_MAX                  128U
#define PADDING_VAL                 0xFFU

void uds_recv_frame(uint32_t id, uint8_t *frame_buf, uint8_t frame_dlc);
int uds_send_frame(uint32_t response_id, uint8_t *frame_buf, uint8_t frame_dlc);
void uds_init(void);
void uds_1ms_task(void);
void uds_request_reset_after_tx(void);
uint32_t uds_port_get_time_ms(void);
uint8_t uds_reset_after_tx_pending(void);

#endif
