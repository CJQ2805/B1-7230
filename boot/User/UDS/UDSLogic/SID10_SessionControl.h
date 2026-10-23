#ifndef BOOT_UDS_SID10_SESSION_CONTROL_H
#define BOOT_UDS_SID10_SESSION_CONTROL_H

#include "uds_service.h"

bool_t service_10_check_len(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_10_SessionControl(const uint8_t *msg_buf, uint16_t msg_dlc);
void set_current_session(uds_session_t session);
uds_session_t get_current_session(void);

#endif
