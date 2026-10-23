#ifndef BOOT_UDS_SID28_COMMUNICATION_CONTROL_H
#define BOOT_UDS_SID28_COMMUNICATION_CONTROL_H

#include "uds_service.h"

bool_t service_28_check_len(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_28_CommunicationControl(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_28_reset(void);
void service_28_timeout_task_1ms(void);
void service_28_on_s3_timeout(void);

#endif
