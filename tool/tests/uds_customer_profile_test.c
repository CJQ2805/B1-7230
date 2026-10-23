#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../../app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/uds_customer_profile.h"

static void test_crc16_ccitt_false_vectors(void)
{
    static const uint8_t check_text[] = "123456789";
    uint8_t sequence[256];
    uint16_t i;

    assert(uds_customer_profile_crc16(check_text, 9U) == 0x29B1U);
    for (i = 0U; i < 256U; ++i)
    {
        sequence[i] = (uint8_t)i;
    }
    assert(uds_customer_profile_crc16(sequence, sizeof(sequence)) == 0x3FBDU);

    /* 固定 FlashDriver 识别载荷 BMS-DUCAR 的 CRC16，线上按高字节、低字节发送。 */
    assert(uds_customer_profile_crc16(uds_customer_flash_driver_id,
                                      UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE) == 0x3981U);
    assert(uds_customer_profile_crc16_update(
               uds_customer_profile_crc16_update(0xFFFFU, &uds_customer_flash_driver_id[0], 4U),
               &uds_customer_flash_driver_id[4], 5U) == 0x3981U);
}

static void test_security_key_algorithm(void)
{
    static const uint8_t seed[4] = {0x12U, 0x34U, 0x56U, 0x78U};
    static const uint8_t zero_key_const[4] = {0U, 0U, 0U, 0U};
    static const uint8_t custom_key_const[4] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    static const uint8_t expected_zero[4] = {0x68U, 0xACU, 0x68U, 0xACU};
    static const uint8_t expected_custom[4] = {0xB4U, 0x52U, 0x78U, 0x8EU};
    uint8_t key[4];
    uint8_t i;

    /* Default customer KeyConst is configurable and currently all-zero. */
    for (i = 0U; i < 4U; ++i)
    {
        assert(uds_customer_key_const[i] == 0U);
    }

    uds_customer_profile_calculate_key(seed, zero_key_const, key);
    assert(key[0] == expected_zero[0]);
    assert(key[1] == expected_zero[1]);
    assert(key[2] == expected_zero[2]);
    assert(key[3] == expected_zero[3]);

    uds_customer_profile_calculate_key(seed, custom_key_const, key);
    assert(key[0] == expected_custom[0]);
    assert(key[1] == expected_custom[1]);
    assert(key[2] == expected_custom[2]);
    assert(key[3] == expected_custom[3]);
}

static void test_flash_driver_signature(void)
{
    static const uint8_t correct[9] = {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'R'};
    static const uint8_t too_short[8] = {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A'};
    static const uint8_t too_long[10] = {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'R', '!'};
    static const uint8_t wrong[9] = {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'X'};

    assert(uds_customer_profile_is_flash_driver(correct, sizeof(correct)) != 0U);
    assert(uds_customer_profile_is_flash_driver(too_short, sizeof(too_short)) == 0U);
    assert(uds_customer_profile_is_flash_driver(too_long, sizeof(too_long)) == 0U);
    assert(uds_customer_profile_is_flash_driver(wrong, sizeof(wrong)) == 0U);
    assert(uds_customer_profile_is_flash_driver(NULL, 9U) == 0U);
}

static void test_fingerprint_format(void)
{
    static const uint8_t valid[8] = {0x20U, 0x26U, 0x09U, 0x23U,
                                     'A', '0', '0', '1'};
    static const uint8_t bad_month[8] = {0x20U, 0x26U, 0x13U, 0x23U,
                                         'A', '0', '0', '1'};
    static const uint8_t bad_ascii[8] = {0x20U, 0x26U, 0x09U, 0x23U,
                                         0x01U, '0', '0', '1'};

    assert(uds_customer_profile_validate_fingerprint(valid, sizeof(valid)) != 0U);
    assert(uds_customer_profile_validate_fingerprint(bad_month, sizeof(bad_month)) == 0U);
    assert(uds_customer_profile_validate_fingerprint(bad_ascii, sizeof(bad_ascii)) == 0U);
    assert(uds_customer_profile_validate_fingerprint(valid, 7U) == 0U);
}

int main(void)
{
    test_crc16_ccitt_false_vectors();
    test_security_key_algorithm();
    test_flash_driver_signature();
    test_fingerprint_format();
    return 0;
}
