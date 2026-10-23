#ifndef TEST_UDS_PORT_H
#define TEST_UDS_PORT_H

#include <stdbool.h>
#include <stdint.h>

void uds_1ms_task(void);
void uds_init(void);
bool uds_normal_receive_enabled(void);
void uds_recv_frame(uint32_t id, const uint8_t *data, uint8_t len);

#endif
