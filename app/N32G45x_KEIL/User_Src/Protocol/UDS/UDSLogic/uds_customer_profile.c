#include "uds_customer_profile.h"

#include <stddef.h>

/* 固定下载识别文件的唯一内容：ASCII "BMS-DUCAR"，不包含可执行指令。 */
const uint8_t uds_customer_flash_driver_id[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE] =
{
    0x42U, 0x4DU, 0x53U, 0x2DU, 0x44U, 0x55U, 0x43U, 0x41U, 0x52U
};

#if (UDS_CUSTOMER_KEY_CONST_VALID == 1U)
const uint8_t uds_customer_key_const[4] =
{
    UDS_CUSTOMER_KEY_CONST_0,
    UDS_CUSTOMER_KEY_CONST_1,
    UDS_CUSTOMER_KEY_CONST_2,
    UDS_CUSTOMER_KEY_CONST_3
};
#endif

/* Customer PDF: CRC-16/CCITT-FALSE, poly=0x1021, init=0xFFFF, no reflection/xorout. */
uint16_t uds_customer_profile_crc16(const uint8_t *data, uint32_t length)
{
    return uds_customer_profile_crc16_update(0xFFFFU, data, length);
}

uint16_t uds_customer_profile_crc16_update(uint16_t crc,
                                          const uint8_t *data,
                                          uint32_t length)
{
    uint32_t byte_index;
    uint8_t bit_index;

    if ((data == NULL) && (length != 0UL))
    {
        return 0U;
    }

    for (byte_index = 0UL; byte_index < length; ++byte_index)
    {
        crc ^= (uint16_t)((uint16_t)data[byte_index] << 8);
        for (bit_index = 0U; bit_index < 8U; ++bit_index)
        {
            if ((crc & 0x8000U) != 0U)
            {
                crc = (uint16_t)((crc << 1) ^ 0x1021U);
            }
            else
            {
                crc = (uint16_t)(crc << 1);
            }
        }
    }

    return crc;
}

/* Customer §7.4: T1=Seed^K; T2=rotate(Seed)^K; Key=(T1+T2) mod 256. */
void uds_customer_profile_calculate_key(const uint8_t seed[4],
                                        const uint8_t key_const[4],
                                        uint8_t key[4])
{
    static const uint8_t rotated_source[4] = {2U, 3U, 0U, 1U};
    uint8_t index;

    if ((seed == NULL) || (key_const == NULL) || (key == NULL))
    {
        return;
    }

    for (index = 0U; index < 4U; ++index)
    {
        uint8_t t1 = (uint8_t)(seed[index] ^ key_const[index]);
        uint8_t t2 = (uint8_t)(seed[rotated_source[index]] ^ key_const[index]);
        key[index] = (uint8_t)((uint16_t)t1 + (uint16_t)t2);
    }
}

uint8_t uds_customer_profile_is_flash_driver(const uint8_t *data, uint16_t length)
{
    uint8_t index;

    if ((data == NULL) || (length != UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE))
    {
        return 0U;
    }

    for (index = 0U; index < UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE; ++index)
    {
        if (data[index] != uds_customer_flash_driver_id[index])
        {
            return 0U;
        }
    }

    return 1U;
}

static uint8_t uds_customer_profile_is_bcd(uint8_t value)
{
    return (((value >> 4) <= 9U) && ((value & 0x0FU) <= 9U)) ? 1U : 0U;
}

uint8_t uds_customer_profile_validate_fingerprint(const uint8_t *data, uint16_t length)
{
    uint8_t index;
    uint8_t month;
    uint8_t day;

    if ((data == NULL) || (length != UDS_CUSTOMER_FINGERPRINT_SIZE))
    {
        return 0U;
    }

    if ((uds_customer_profile_is_bcd(data[0]) == 0U) ||
        (uds_customer_profile_is_bcd(data[1]) == 0U) ||
        (uds_customer_profile_is_bcd(data[2]) == 0U) ||
        (uds_customer_profile_is_bcd(data[3]) == 0U))
    {
        return 0U;
    }

    month = (uint8_t)(((data[2] >> 4) * 10U) + (data[2] & 0x0FU));
    day = (uint8_t)(((data[3] >> 4) * 10U) + (data[3] & 0x0FU));
    if ((month < 1U) || (month > 12U) || (day < 1U) || (day > 31U))
    {
        return 0U;
    }

    for (index = 4U; index < UDS_CUSTOMER_FINGERPRINT_SIZE; ++index)
    {
        /* .zflash当前指纹将未配置的4字节设备号填为00，按ASCII NUL接收。 */
        if ((data[index] != 0U) &&
            ((data[index] < 0x20U) || (data[index] > 0x7EU)))
        {
            return 0U;
        }
    }

    return 1U;
}
