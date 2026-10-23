#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../../app/N32G45x_KEIL/User_Src/App/app_flash_map.h"
#include "../../app/N32G45x_KEIL/User_Src/Driver/d_flash.h"
#include "../../app/N32G45x_KEIL/User_Src/Protocol/IAP/ducar_uart_iap_protocol.h"
#include "../../app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/uds_customer_profile.h"
#include "../../app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/uds_upgrade.h"

uint8_t uds_upgrade_test_app2[APP_FLASH_MAP_APP2_END_ADDR -
                              APP_FLASH_MAP_APP2_START_ADDR];

static uint8_t test_fingerprint_page[APP_FLASH_MAP_FLASH_PAGE_SIZE];
static uint32_t app2_erase_count;
static uint32_t app2_write_count;
static uint32_t prepared_image_size;
static uint32_t prepared_image_crc32;
static uint8_t boot_handoff_prepared;

#define TEST_DUCAR_HEADER_SIZE 128U

static void test_write_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t test_align_up_128(uint32_t value)
{
    return (value + 127UL) & ~127UL;
}

static void test_build_ducar_package(uint8_t *package,
                                     const uint8_t *image,
                                     uint32_t image_size)
{
    uint32_t package_payload_size = test_align_up_128(image_size);

    memset(package, 0xFF, TEST_DUCAR_HEADER_SIZE + package_payload_size);
    memcpy(package, "DUCAR", 5U);
    memcpy(&package[17], "A000000000", 10U);
    test_write_le32(&package[31], ducar_iap_crc32_calc(image, image_size));
    test_write_le32(&package[35], image_size);
    memcpy(&package[TEST_DUCAR_HEADER_SIZE], image, image_size);
}

static void test_enter_driver_verified(const uint8_t *fingerprint,
                                      const uint8_t *marker)
{
    bool_t crc_matches;

    memset(uds_upgrade_test_app2, 0xFF, sizeof(uds_upgrade_test_app2));
    memset(test_fingerprint_page, 0xFF, sizeof(test_fingerprint_page));
    app2_erase_count = 0UL;
    app2_write_count = 0UL;
    boot_handoff_prepared = 0U;
    uds_upgrade_reset_state();

    assert(uds_upgrade_store_fingerprint(fingerprint,
                                         UDS_CUSTOMER_FINGERPRINT_SIZE) == UDS_UPGRADE_OK);
    assert(uds_upgrade_request_download(0UL,
                                       UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, marker,
                                     UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    assert(uds_upgrade_validate_transfer_crc16(0x3981U, &crc_matches) == UDS_UPGRADE_OK);
    assert(crc_matches == TRUE);
}

bool d_flash_erase_page(uint32_t page_addr)
{
    if (page_addr == APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR)
    {
        memset(test_fingerprint_page, 0xFF, sizeof(test_fingerprint_page));
        return true;
    }
    if ((page_addr >= APP_FLASH_MAP_APP2_START_ADDR) &&
        ((page_addr + D_FLASH_PAGE_SIZE) <= APP_FLASH_MAP_APP2_END_ADDR))
    {
        memset(&uds_upgrade_test_app2[page_addr - APP_FLASH_MAP_APP2_START_ADDR],
               0xFF,
               D_FLASH_PAGE_SIZE);
        ++app2_erase_count;
        return true;
    }
    return false;
}

bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t padded_len = (len + 3UL) & ~3UL;
    uint32_t offset;

    if ((data == NULL) || (len == 0UL))
    {
        return false;
    }
    if ((addr >= APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR) &&
        ((addr + padded_len) <=
         (APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR + sizeof(test_fingerprint_page))))
    {
        offset = addr - APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR;
        memcpy(&test_fingerprint_page[offset], data, len);
        if (padded_len > len)
        {
            memset(&test_fingerprint_page[offset + len], 0xFF, padded_len - len);
        }
        return true;
    }
    if ((addr >= APP_FLASH_MAP_APP2_START_ADDR) &&
        ((addr + padded_len) <= APP_FLASH_MAP_APP2_END_ADDR))
    {
        offset = addr - APP_FLASH_MAP_APP2_START_ADDR;
        memcpy(&uds_upgrade_test_app2[offset], data, len);
        if (padded_len > len)
        {
            memset(&uds_upgrade_test_app2[offset + len], 0xFF, padded_len - len);
        }
        ++app2_write_count;
        return true;
    }
    return false;
}

uint32_t ducar_iap_crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t byte_index;
    uint8_t bit_index;

    if ((data == NULL) && (len > 0UL))
    {
        return 0UL;
    }
    for (byte_index = 0UL; byte_index < len; ++byte_index)
    {
        crc ^= data[byte_index];
        for (bit_index = 0U; bit_index < 8U; ++bit_index)
        {
            crc = ((crc & 1UL) != 0UL) ?
                  ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
        }
    }
    return ~crc;
}

const char *app_param_get_software_pn(void)
{
    return "A000000000";
}

bool ducar_iap_part_number_is_allowed(const char *local_part_number,
                                     const char *firmware_part_number)
{
    if ((local_part_number == NULL) || (firmware_part_number == NULL))
    {
        return false;
    }
    return (strncmp(firmware_part_number, "A000000000", 10U) == 0) ||
           (strncmp(firmware_part_number, local_part_number, 10U) == 0);
}

bool ducar_iap_prepare_boot_move(uint32_t image_size, uint32_t image_crc32)
{
    prepared_image_size = image_size;
    prepared_image_crc32 = image_crc32;
    boot_handoff_prepared = 1U;
    return true;
}

static void test_customer_upgrade_happy_path(void)
{
    static const uint8_t fingerprint[UDS_CUSTOMER_FINGERPRINT_SIZE] =
        {0x20U, 0x26U, 0x10U, 0x09U, 'D', 'U', 'C', 'A'};
    static const uint8_t marker[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE] =
        {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'R'};
    uint8_t image[130U];
    uint8_t package[TEST_DUCAR_HEADER_SIZE + 256U];
    uint8_t changed_header[TEST_DUCAR_HEADER_SIZE];
    uint16_t image_crc16;
    bool_t crc_matches;
    uint8_t i;

    for (i = 0U; i < sizeof(image); ++i)
    {
        image[i] = (uint8_t)(i ^ 0x5AU);
    }
    test_build_ducar_package(package, image, sizeof(image));
    memset(uds_upgrade_test_app2, 0xFF, sizeof(uds_upgrade_test_app2));
    memset(test_fingerprint_page, 0xFF, sizeof(test_fingerprint_page));
    app2_erase_count = 0UL;
    app2_write_count = 0UL;
    boot_handoff_prepared = 0U;
    uds_upgrade_reset_state();

    assert(uds_upgrade_request_download(APP_FLASH_MAP_APP1_START_ADDR, 4UL) ==
           NRC_CONDITIONS_NOT_CORRECT);
    assert(uds_upgrade_store_fingerprint(fingerprint, sizeof(fingerprint)) == UDS_UPGRADE_OK);
    assert(test_fingerprint_page[0] == 'F' && test_fingerprint_page[1] == 'P');
    assert(memcmp(&test_fingerprint_page[2], fingerprint, sizeof(fingerprint)) == 0);

    /* BMS-DUCAR is buffered for recognition and must not be written into APP2. */
    assert(uds_upgrade_request_download(0UL, 8UL) == NRC_REQUEST_OUT_OF_RANGE);
    assert(uds_upgrade_request_download(0UL, sizeof(marker)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, marker, sizeof(marker)) == UDS_UPGRADE_OK);
    assert(app2_write_count == 0UL);
    assert(uds_upgrade_transfer_data(1U, marker, sizeof(marker)) == UDS_UPGRADE_OK);
    {
        static const uint8_t changed_marker[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE] =
            {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'X'};
        assert(uds_upgrade_transfer_data(1U, changed_marker,
                                         sizeof(changed_marker)) ==
               NRC_WRONG_BLOCK_SEQUENCE_COUNTER);
    }
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    assert(uds_upgrade_validate_transfer_crc16(0U, &crc_matches) == UDS_UPGRADE_OK);
    assert(crc_matches == FALSE);

    /* Re-send the marker after a bad checksum; only exact BMS-DUCAR plus CRC passes. */
    assert(uds_upgrade_request_download(0UL, sizeof(marker)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, marker, sizeof(marker)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    assert(uds_upgrade_validate_transfer_crc16(0x3981U, &crc_matches) == UDS_UPGRADE_OK);
    assert(crc_matches == TRUE);

    assert(uds_upgrade_validate_erase_request(APP_FLASH_MAP_APP1_START_ADDR + 4UL,
                                               sizeof(image)) ==
           NRC_REQUEST_OUT_OF_RANGE);
    /* FF00 declares the real APP byte count; 34 declares wrapped BIN length. */
    assert(uds_upgrade_erase_application(APP_FLASH_MAP_APP1_START_ADDR,
                                          sizeof(image)) == UDS_UPGRADE_OK);
    assert(app2_erase_count == 1UL);
    assert(uds_upgrade_request_download(APP_FLASH_MAP_APP1_START_ADDR,
                                         sizeof(package)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(2U, package, 128U) ==
           NRC_WRONG_BLOCK_SEQUENCE_COUNTER);
    assert(uds_upgrade_transfer_data(1U, package, 128U) == UDS_UPGRADE_OK);
    assert(app2_write_count == 0UL);
    assert(uds_upgrade_test_app2[0] == 0xFFU);
    assert(uds_upgrade_transfer_data(1U, package, 128U) == UDS_UPGRADE_OK);
    memcpy(changed_header, package, sizeof(changed_header));
    changed_header[0] ^= 0x01U;
    assert(uds_upgrade_transfer_data(1U, changed_header,
                                     sizeof(changed_header)) ==
           NRC_WRONG_BLOCK_SEQUENCE_COUNTER);
    assert(uds_upgrade_transfer_data(2U, &package[128], 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(3U, &package[256], 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);
    image_crc16 = uds_customer_profile_crc16(package, sizeof(package));
    assert(uds_upgrade_validate_transfer_crc16(image_crc16, &crc_matches) ==
           UDS_UPGRADE_OK);
    assert(crc_matches == TRUE);
    assert(app2_write_count == 2UL);
    assert(memcmp(uds_upgrade_test_app2, image, sizeof(image)) == 0);
    assert(uds_upgrade_test_app2[sizeof(image)] == 0xFFU);
    assert(uds_upgrade_check_programming_dependencies() == UDS_UPGRADE_OK);
    assert(uds_upgrade_image_verified() == TRUE);
    assert(uds_upgrade_prepare_boot_move() == TRUE);
    assert(boot_handoff_prepared == 1U);
    assert(prepared_image_size == sizeof(image));
    assert(prepared_image_crc32 == ducar_iap_crc32_calc(image, sizeof(image)));
}

static void test_ducar_crc32_mismatch_rejected_after_crc16_passes(void)
{
    static const uint8_t fingerprint[UDS_CUSTOMER_FINGERPRINT_SIZE] =
        {0x20U, 0x26U, 0x10U, 0x09U, 'D', 'U', 'C', 'A'};
    static const uint8_t marker[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE] =
        {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'R'};
    uint8_t image[130U];
    uint8_t package[TEST_DUCAR_HEADER_SIZE + 256U];
    uint16_t package_crc16;
    bool_t crc_matches;
    uint8_t i;

    for (i = 0U; i < sizeof(image); ++i)
    {
        image[i] = (uint8_t)(i ^ 0xA5U);
    }
    test_build_ducar_package(package, image, sizeof(image));
    package[31] ^= 0x01U; /* Wrong DUCAR payload CRC32, but keep package CRC16 valid. */
    test_enter_driver_verified(fingerprint, marker);

    assert(uds_upgrade_erase_application(APP_FLASH_MAP_APP1_START_ADDR,
                                          sizeof(image)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_request_download(APP_FLASH_MAP_APP1_START_ADDR,
                                         sizeof(package)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, package, 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(2U, &package[128], 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(3U, &package[256], 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_exit() == UDS_UPGRADE_OK);

    package_crc16 = uds_customer_profile_crc16(package, sizeof(package));
    assert(uds_upgrade_validate_transfer_crc16(package_crc16, &crc_matches) ==
           UDS_UPGRADE_OK);
    assert(crc_matches == FALSE);
    assert(uds_upgrade_check_programming_dependencies() == NRC_CONDITIONS_NOT_CORRECT);
    assert(uds_upgrade_image_verified() == FALSE);
    assert(boot_handoff_prepared == 0U);
}

static void test_non_ff_package_padding_is_rejected(void)
{
    static const uint8_t fingerprint[UDS_CUSTOMER_FINGERPRINT_SIZE] =
        {0x20U, 0x26U, 0x10U, 0x09U, 'D', 'U', 'C', 'A'};
    static const uint8_t marker[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE] =
        {'B', 'M', 'S', '-', 'D', 'U', 'C', 'A', 'R'};
    uint8_t image[130U];
    uint8_t package[TEST_DUCAR_HEADER_SIZE + 256U];
    uint8_t i;

    for (i = 0U; i < sizeof(image); ++i)
    {
        image[i] = (uint8_t)(i ^ 0x3CU);
    }
    test_build_ducar_package(package, image, sizeof(image));
    package[TEST_DUCAR_HEADER_SIZE + sizeof(image)] = 0x00U;
    test_enter_driver_verified(fingerprint, marker);

    assert(uds_upgrade_erase_application(APP_FLASH_MAP_APP1_START_ADDR,
                                          sizeof(image)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_request_download(APP_FLASH_MAP_APP1_START_ADDR,
                                         sizeof(package)) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(1U, package, 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(2U, &package[128], 128U) == UDS_UPGRADE_OK);
    assert(uds_upgrade_transfer_data(3U, &package[256], 128U) ==
           NRC_REQUEST_OUT_OF_RANGE);
    assert(uds_upgrade_image_verified() == FALSE);
    assert(uds_upgrade_check_programming_dependencies() == NRC_CONDITIONS_NOT_CORRECT);
    assert(boot_handoff_prepared == 0U);
}

int main(void)
{
    static const uint8_t crc32_test_vector[] = "123456789";

    assert(ducar_iap_crc32_calc(crc32_test_vector,
                                sizeof(crc32_test_vector) - 1U) == 0xCBF43926UL);
    test_customer_upgrade_happy_path();
    test_ducar_crc32_mismatch_rejected_after_crc16_passes();
    test_non_ff_package_padding_is_rejected();
    return 0;
}
