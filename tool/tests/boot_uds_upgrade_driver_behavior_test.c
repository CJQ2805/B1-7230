#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Boot/boot_upgrade.h"
#include "Hal_Driver/boot_flash.h"
#include "uds_customer_profile.h"
#include "uds_upgrade.h"

static uint32_t s_flash_write_count;
static uint32_t s_last_write_address;
static uint32_t s_last_write_length;
static uint32_t s_driver_recognition_count;

bool boot_flash_erase_page(uint32_t page_addr)
{
    (void)page_addr;
    return true;
}

bool boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    (void)data;
    ++s_flash_write_count;
    s_last_write_address = addr;
    s_last_write_length = len;
    return true;
}

bool boot_flash_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    (void)addr;
    (void)data;
    (void)len;
    return true;
}

bool boot_flash_is_blank(uint32_t addr, uint32_t len)
{
    (void)addr;
    (void)len;
    return true;
}

uint32_t boot_crc32(const uint8_t *data, uint32_t len)
{
    (void)data;
    (void)len;
    return 0x12345678UL;
}

bool boot_app_app1_is_valid(void)
{
    return true;
}

bool boot_upgrade_prepare_app1(uint32_t image_size, uint32_t image_crc32)
{
    (void)image_size;
    (void)image_crc32;
    return true;
}

uint8_t uds_customer_profile_validate_fingerprint(const uint8_t *data,
                                                  uint16_t length)
{
    return ((data != NULL) && (length == UDS_CUSTOMER_FINGERPRINT_SIZE)) ? 1U : 0U;
}

uint8_t uds_customer_profile_is_flash_driver(const uint8_t *data,
                                             uint16_t length)
{
    (void)data;
    (void)length;
    ++s_driver_recognition_count;
    return 0U;
}

uint16_t uds_customer_profile_crc16_update(uint16_t crc,
                                           const uint8_t *data,
                                           uint32_t length)
{
    uint32_t i;
    for (i = 0U; i < length; ++i)
    {
        crc = (uint16_t)(crc + data[i]);
    }
    return crc;
}

int main(void)
{
    static const uint8_t fingerprint[UDS_CUSTOMER_FINGERPRINT_SIZE] =
        {0x20U, 0x24U, 0x05U, 0x27U, 0U, 0U, 0U, 0U};
    static const uint8_t driver_bytes[] = {0x00U, 0xA5U, 0x5AU};
    static const uint8_t app_bytes[8] =
        {0x00U, 0x00U, 0x02U, 0x20U, 0xADU, 0x01U, 0x01U, 0x08U};
    bool_t crc_matches = FALSE;
    uint16_t app_crc;
    uint32_t writes_after_fingerprint;

    uds_upgrade_reset_state();
    assert(uds_upgrade_store_fingerprint(fingerprint, sizeof(fingerprint)) == UDS_UPGRADE_OK);
    writes_after_fingerprint = s_flash_write_count;

    /* S19 data starts at address zero; BOOT receives it without mapping it into SRAM. */
    assert(uds_upgrade_request_download(0UL, sizeof(driver_bytes)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, driver_bytes, sizeof(driver_bytes)) == UDS_UPGRADE_OK);
    assert(s_flash_write_count == writes_after_fingerprint);
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    assert(uds_upgrade_validate_transfer_crc16(0xBEEFU, &crc_matches) == UDS_UPGRADE_OK);
    assert(crc_matches == TRUE);
    assert(s_driver_recognition_count == 0U);

    /* APP bytes still go directly to APP1 and keep their own CRC validation. */
    assert(uds_upgrade_request_download(BOOT_APP1_START_ADDR, sizeof(app_bytes)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, app_bytes, sizeof(app_bytes)) == UDS_UPGRADE_OK);
    assert(s_last_write_address == BOOT_APP1_START_ADDR);
    assert(s_last_write_length == sizeof(app_bytes));
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    app_crc = uds_customer_profile_crc16_update(0xFFFFU, app_bytes, sizeof(app_bytes));
    crc_matches = FALSE;
    assert(uds_upgrade_validate_transfer_crc16(app_crc, &crc_matches) == UDS_UPGRADE_OK);
    assert(crc_matches == TRUE);
    assert(uds_upgrade_check_programming_dependencies() == UDS_UPGRADE_OK);
    assert(uds_upgrade_image_verified() == TRUE);
    assert(uds_upgrade_mark_app1_ready() == TRUE);

    puts("BOOT FlashDriver receive-only and APP1 CRC behavior: PASS");
    return 0;
}
