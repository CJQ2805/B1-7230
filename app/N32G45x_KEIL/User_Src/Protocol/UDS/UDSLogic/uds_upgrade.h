/******************************************************************************
 * Customer UDS programming-session state manager.
 *
 * The nine-byte BMS-DUCAR FlashDriver transfer is an identification token only.
 * Application bytes are staged into APP2 and handed to the existing BOOT move
 * mechanism only after the customer CRC16/dependency routines have passed.
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
/* For APP, image_size is the complete DUCAR BIN length; FF00 carries raw APP size. */
uint8_t uds_upgrade_request_download(uint32_t target_address, uint32_t download_size);
uint8_t uds_upgrade_transfer_data(uint8_t block_counter,
                                  const uint8_t *data,
                                  uint16_t data_len);
uint8_t uds_upgrade_transfer_exit(void);
uint8_t uds_upgrade_validate_transfer_crc16(uint16_t expected_crc16,
                                             bool_t *crc_matches);
/* FF00 sizes and erases raw APP bytes, not the wrapper/padded UDS BIN length. */
uint8_t uds_upgrade_validate_erase_request(uint32_t target_address,
                                            uint32_t image_size);
uint8_t uds_upgrade_erase_application(uint32_t target_address,
                                       uint32_t image_size);
uint8_t uds_upgrade_check_programming_dependencies(void);
bool_t uds_upgrade_image_verified(void);
bool_t uds_upgrade_prepare_boot_move(void);

#endif
