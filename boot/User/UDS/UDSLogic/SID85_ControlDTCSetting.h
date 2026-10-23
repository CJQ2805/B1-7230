#ifndef BOOT_UDS_SID85_CONTROL_DTC_SETTING_H
#define BOOT_UDS_SID85_CONTROL_DTC_SETTING_H

#include "uds_service.h"

bool_t service_85_check_len(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_85_ControlDTCSetting(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_85_reset(void);
bool_t service_85_is_enabled(void);

#endif
