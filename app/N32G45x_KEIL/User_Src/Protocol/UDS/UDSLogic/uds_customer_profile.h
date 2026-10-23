#ifndef UDS_CUSTOMER_PROFILE_H
#define UDS_CUSTOMER_PROFILE_H

#include <stdint.h>

#define UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE 9U
#define UDS_CUSTOMER_FINGERPRINT_SIZE     8U
#define UDS_CUSTOMER_KEY_CONST_VALID      1U /* 已根据 SeednKey_20250401.dll 的导出函数确认。 */

extern const uint8_t uds_customer_flash_driver_id[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE];

uint16_t uds_customer_profile_crc16(const uint8_t *data, uint32_t length);
uint16_t uds_customer_profile_crc16_update(uint16_t crc,
                                          const uint8_t *data,
                                          uint32_t length);
void uds_customer_profile_calculate_key(const uint8_t seed[4],
                                        const uint8_t key_const[4],
                                        uint8_t key[4]);
const uint8_t *uds_customer_profile_get_key_const(uint8_t security_level);
uint8_t uds_customer_profile_is_flash_driver(const uint8_t *data, uint16_t length);
uint8_t uds_customer_profile_validate_fingerprint(const uint8_t *data, uint16_t length);

#endif
