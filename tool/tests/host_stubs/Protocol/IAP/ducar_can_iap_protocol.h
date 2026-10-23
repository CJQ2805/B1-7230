#ifndef TEST_DUCAR_CAN_IAP_PROTOCOL_H
#define TEST_DUCAR_CAN_IAP_PROTOCOL_H

#include <stdbool.h>
#include "Driver/d_can.h"

void ducar_can_iap_protocol_init(void);
bool ducar_can_iap_protocol_is_packet(const t_can_packet *packet);
bool ducar_can_iap_protocol_is_busy(void);
void ducar_can_iap_protocol_receive(const t_can_packet *packet);
void ducar_can_iap_protocol_task(void);
bool ducar_iap_is_busy(void);

#endif
