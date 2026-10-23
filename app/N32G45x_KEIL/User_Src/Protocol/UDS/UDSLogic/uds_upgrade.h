/******************************************************************************
 * UDS upgrade interface shared with the BOOT-owned customer flow.
 *
 * The active BOOT implementation accepts the configured FlashDriver transfer
 * without interpreting its bytes, then writes the bare APP S19 data directly
 * to APP1 and checks the APP CRC before marking APP1 ready.
 ******************************************************************************/
#ifndef UDS_UPGRADE_H
#define UDS_UPGRADE_H

#include <stdint.h>
#include "uds_service.h"

#define UDS_UPGRADE_BLOCK_DATA_MAX       128U
#define UDS_UPGRADE_OK                   ((uint8_t)0U)
#define UDS_UPGRADE_ROUTINE_STATUS_OK    0x04U
#define UDS_UPGRADE_ROUTINE_STATUS_FAIL  0x05U

void uds_upgrade_reset_state(void);
uint8_t uds_upgrade_store_fingerprint(const uint8_t *data, uint16_t length);
/* RequestDownload begins one customer-flow file transfer at its S-record address. */
uint8_t uds_upgrade_request_download(uint32_t target_address, uint32_t download_size);
uint8_t uds_upgrade_transfer_data(uint8_t block_counter,
                                  const uint8_t *data,
                                  uint16_t data_len);
uint8_t uds_upgrade_transfer_exit(void);
uint8_t uds_upgrade_validate_transfer_crc16(uint16_t expected_crc16,
                                             bool_t *crc_matches);
/* Optional 31 FF00 erases the declared APP1 range; ZCANPRO currently erases on write. */
uint8_t uds_upgrade_validate_erase_request(uint32_t target_address,
                                            uint32_t image_size);
uint8_t uds_upgrade_erase_application(uint32_t target_address,
                                       uint32_t image_size);
uint8_t uds_upgrade_check_programming_dependencies(void);
bool_t uds_upgrade_image_verified(void);
/* Legacy APP2 interface retained for its excluded APP-local implementation/tests. */
bool_t uds_upgrade_prepare_boot_move(void);
/* BOOT's shared SID 11 handler records an APP1-ready marker; APP data is not copied. */
bool_t uds_upgrade_mark_app1_ready(void);

#endif
