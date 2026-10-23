#ifndef TEST_ZS_B1_CAN_PROTOCOL_H
#define TEST_ZS_B1_CAN_PROTOCOL_H

#include <stdbool.h>

void zs_b1_can_protocol_init(void);
void zs_b1_can_protocol_task(void);
bool zs_b1_can_protocol_allows_sleep(void);

#endif
