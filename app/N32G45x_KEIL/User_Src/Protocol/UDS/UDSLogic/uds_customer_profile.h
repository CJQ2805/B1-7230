#ifndef UDS_CUSTOMER_PROFILE_H
#define UDS_CUSTOMER_PROFILE_H

#include <stdint.h>

#define UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE 9U
#define UDS_CUSTOMER_FINGERPRINT_SIZE     8U
#define UDS_CUSTOMER_KEY_CONST_VALID      0U /* DLL中的KeyConst尚未静态确认，安全访问必须拒绝解锁。 */

#if (UDS_CUSTOMER_KEY_CONST_VALID == 1U) && \
    (!defined(UDS_CUSTOMER_KEY_CONST_0) || !defined(UDS_CUSTOMER_KEY_CONST_1) || \
     !defined(UDS_CUSTOMER_KEY_CONST_2) || !defined(UDS_CUSTOMER_KEY_CONST_3))
#error "Set the four verified customer KeyConst bytes before enabling UDS security access."
#endif

extern const uint8_t uds_customer_flash_driver_id[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE];
#if (UDS_CUSTOMER_KEY_CONST_VALID == 1U)
extern const uint8_t uds_customer_key_const[4];
#endif

uint16_t uds_customer_profile_crc16(const uint8_t *data, uint32_t length);
uint16_t uds_customer_profile_crc16_update(uint16_t crc,
                                          const uint8_t *data,
                                          uint32_t length);
void uds_customer_profile_calculate_key(const uint8_t seed[4],
                                        const uint8_t key_const[4],
                                        uint8_t key[4]);
uint8_t uds_customer_profile_is_flash_driver(const uint8_t *data, uint16_t length);
uint8_t uds_customer_profile_validate_fingerprint(const uint8_t *data, uint16_t length);

#endif
