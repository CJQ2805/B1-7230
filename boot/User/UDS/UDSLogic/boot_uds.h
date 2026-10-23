#ifndef BOOT_UDS_H
#define BOOT_UDS_H

#include <stdint.h>

void boot_uds_task_1ms(void);
void boot_uds_receive_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

#endif
