#ifndef BOOT_UDS_SID22_READ_DATA_BY_IDENTIFIER_H
#define BOOT_UDS_SID22_READ_DATA_BY_IDENTIFIER_H

#include "uds_service.h"

bool_t service_22_check_len(const uint8_t *msg_buf, uint16_t msg_dlc);
void service_22_ReadDataByIdentifier(const uint8_t *msg_buf, uint16_t msg_dlc);

#endif
