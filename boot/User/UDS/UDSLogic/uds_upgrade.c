#include "uds_upgrade.h"

#include <string.h>

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Boot/boot_upgrade.h"
#include "Hal_Driver/boot_crc.h"
#include "Hal_Driver/boot_flash.h"
#include "uds_customer_profile.h"

#define BOOT_UDS_FINGERPRINT_ADDR       ((uint32_t)0x08040800UL)
#define BOOT_UDS_FINGERPRINT_TAG        ((uint16_t)0x5046U)

typedef enum
{
    BOOT_UDS_UPGRADE_IDLE = 0U,
    BOOT_UDS_UPGRADE_DRIVER_RX,
    BOOT_UDS_UPGRADE_DRIVER_EXITED,
    BOOT_UDS_UPGRADE_DRIVER_OK,
    BOOT_UDS_UPGRADE_APP_ERASED,
    BOOT_UDS_UPGRADE_APP_RX,
    BOOT_UDS_UPGRADE_APP_EXITED,
    BOOT_UDS_UPGRADE_APP_OK,
    BOOT_UDS_UPGRADE_DEPENDENCIES_OK,
    BOOT_UDS_UPGRADE_APP1_READY
} boot_uds_upgrade_state_t;

typedef enum
{
    BOOT_UDS_TRANSFER_NONE = 0U,
    BOOT_UDS_TRANSFER_DRIVER,
    BOOT_UDS_TRANSFER_APP
} boot_uds_transfer_type_t;

static boot_uds_upgrade_state_t s_state;
static boot_uds_transfer_type_t s_transfer_type;
static uint32_t s_target_address;
static uint32_t s_transfer_size;
static uint32_t s_received_size;
static uint32_t s_app_size;
static uint32_t s_expected_app_address;
static uint32_t s_expected_app_size;
static uint32_t s_expected_crc32;
static uint32_t s_next_page_to_erase;
static uint16_t s_transfer_crc16;
static uint8_t s_expected_bsc;
static uint8_t s_last_bsc;
static uint16_t s_last_len;
static uint8_t s_last_data[UDS_UPGRADE_BLOCK_DATA_MAX];
static uint8_t s_last_valid;
static uint8_t s_driver_received;
static uint8_t s_fingerprint_written;

static void boot_uds_upgrade_clear_transfer(void)
{
    s_transfer_type = BOOT_UDS_TRANSFER_NONE;
    s_target_address = 0UL;
    s_transfer_size = 0UL;
    s_received_size = 0UL;
    s_transfer_crc16 = 0xFFFFU;
    s_expected_bsc = 1U;
    s_last_bsc = 0U;
    s_last_len = 0U;
    s_last_valid = 0U;
    memset(s_last_data, 0, sizeof(s_last_data));
}

void uds_upgrade_reset_state(void)
{
    s_state = BOOT_UDS_UPGRADE_IDLE;
    s_app_size = 0UL;
    s_expected_app_address = 0UL;
    s_expected_app_size = 0UL;
    s_expected_crc32 = 0UL;
    s_next_page_to_erase = BOOT_APP1_START_ADDR;
    s_driver_received = 0U;
    s_fingerprint_written = 0U;
    boot_uds_upgrade_clear_transfer();
}

uint8_t uds_upgrade_store_fingerprint(const uint8_t *data, uint16_t length)
{
    uint8_t record[2U + UDS_CUSTOMER_FINGERPRINT_SIZE];

    s_fingerprint_written = 0U;
    if (!uds_customer_profile_validate_fingerprint(data, length))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    record[0] = (uint8_t)(BOOT_UDS_FINGERPRINT_TAG & 0xFFU);
    record[1] = (uint8_t)(BOOT_UDS_FINGERPRINT_TAG >> 8);
    memcpy(&record[2], data, UDS_CUSTOMER_FINGERPRINT_SIZE);
    if (!boot_flash_erase_page(BOOT_UDS_FINGERPRINT_ADDR) ||
        !boot_flash_write(BOOT_UDS_FINGERPRINT_ADDR, record, sizeof(record)))
    {
        return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
    }
    s_fingerprint_written = 1U;
    return UDS_UPGRADE_OK;
}

static uint8_t boot_uds_upgrade_begin_transfer(boot_uds_transfer_type_t type,
                                               uint32_t target_address,
                                               uint32_t size)
{
    s_transfer_type = type;
    s_target_address = target_address;
    s_transfer_size = size;
    s_received_size = 0UL;
    s_transfer_crc16 = 0xFFFFU;
    s_expected_bsc = 1U;
    s_last_bsc = 0U;
    s_last_len = 0U;
    s_last_valid = 0U;
    return UDS_UPGRADE_OK;
}

/* The supplied ZCANPRO APP download item has erase_ecu_memory_mode=0 and no explicit FF00 step.
 * Erase APP1 a page at a time immediately before its first write; an explicit FF00 still works. */
static uint8_t boot_uds_upgrade_erase_app_pages_through(uint32_t last_page)
{
    if ((last_page < BOOT_APP1_START_ADDR) ||
        (last_page >= BOOT_APP1_END_ADDR) ||
        ((last_page % BOOT_FLASH_PAGE_SIZE) != 0UL) ||
        (s_next_page_to_erase < BOOT_APP1_START_ADDR) ||
        (s_next_page_to_erase > BOOT_APP1_END_ADDR) ||
        ((s_next_page_to_erase % BOOT_FLASH_PAGE_SIZE) != 0UL))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    while (s_next_page_to_erase <= last_page)
    {
        if (!boot_flash_erase_page(s_next_page_to_erase))
        {
            return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
        }
        s_next_page_to_erase += BOOT_FLASH_PAGE_SIZE;
    }

    return UDS_UPGRADE_OK;
}

uint8_t uds_upgrade_request_download(uint32_t target_address, uint32_t download_size)
{
    if ((download_size == 0UL) || (download_size > 0xFFFFFFFFUL - target_address))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    if (s_state == BOOT_UDS_UPGRADE_IDLE)
    {
        /* ZCANPRO's FlashDriver item is a receive-only protocol placeholder. */
        if ((s_fingerprint_written == 0U) || (s_driver_received != 0U))
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
        (void)boot_uds_upgrade_begin_transfer(BOOT_UDS_TRANSFER_DRIVER,
                                               target_address,
                                               download_size);
        s_state = BOOT_UDS_UPGRADE_DRIVER_RX;
        return UDS_UPGRADE_OK;
    }

    if (s_state == BOOT_UDS_UPGRADE_APP_ERASED)
    {
        if ((target_address != s_expected_app_address) ||
            (download_size != s_expected_app_size) ||
            (target_address < BOOT_APP1_START_ADDR) ||
            (target_address >= BOOT_APP1_END_ADDR) ||
            (download_size > (BOOT_APP1_END_ADDR - target_address)))
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
        (void)boot_uds_upgrade_begin_transfer(BOOT_UDS_TRANSFER_APP,
                                               target_address,
                                               download_size);
        s_state = BOOT_UDS_UPGRADE_APP_RX;
        return UDS_UPGRADE_OK;
    }

    if (s_state == BOOT_UDS_UPGRADE_DRIVER_OK)
    {
        /* Support the delivered .zflash, which does not list a separate SID 31 FF00 request. */
        if ((s_driver_received == 0U) ||
            (target_address != BOOT_APP1_START_ADDR) ||
            (download_size > BOOT_APP_CODE_SIZE))
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
        s_expected_app_address = target_address;
        s_expected_app_size = download_size;
        s_next_page_to_erase = BOOT_APP1_START_ADDR;
        (void)boot_uds_upgrade_begin_transfer(BOOT_UDS_TRANSFER_APP,
                                               target_address,
                                               download_size);
        s_state = BOOT_UDS_UPGRADE_APP_RX;
        return UDS_UPGRADE_OK;
    }

    return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
}

static uint8_t boot_uds_upgrade_duplicate_matches(uint8_t bsc,
                                                  const uint8_t *data,
                                                  uint16_t data_len)
{
    return ((s_last_valid != 0U) && (bsc == s_last_bsc) &&
            (data_len == s_last_len) &&
            (memcmp(data, s_last_data, data_len) == 0)) ? 1U : 0U;
}

uint8_t uds_upgrade_transfer_data(uint8_t block_counter,
                                  const uint8_t *data,
                                  uint16_t data_len)
{
    uint32_t remaining;
    uint16_t expected_len;
    uint32_t destination;
    uint32_t destination_end;
    uint32_t last_page;
    uint8_t erase_result;

    if (((s_state != BOOT_UDS_UPGRADE_DRIVER_RX) &&
         (s_state != BOOT_UDS_UPGRADE_APP_RX)) ||
        (data == 0) || (data_len == 0U) ||
        (data_len > UDS_UPGRADE_BLOCK_DATA_MAX))
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }
    if (s_last_valid != 0U && block_counter == s_last_bsc)
    {
        return boot_uds_upgrade_duplicate_matches(block_counter, data, data_len) ?
               UDS_UPGRADE_OK : (uint8_t)NRC_WRONG_BLOCK_SEQUENCE_COUNTER;
    }
    if (block_counter != s_expected_bsc)
    {
        return (uint8_t)NRC_WRONG_BLOCK_SEQUENCE_COUNTER;
    }

    if (s_received_size > s_transfer_size)
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }
    remaining = s_transfer_size - s_received_size;
    expected_len = (uint16_t)((remaining > UDS_UPGRADE_BLOCK_DATA_MAX) ?
                              UDS_UPGRADE_BLOCK_DATA_MAX : remaining);
    if (data_len != expected_len)
    {
        return (uint8_t)NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT;
    }

    if (s_transfer_type == BOOT_UDS_TRANSFER_DRIVER)
    {
        /* Consume the data block to complete the configured ZCANPRO step. */
    }
    else if (s_transfer_type == BOOT_UDS_TRANSFER_APP)
    {
        destination = BOOT_APP1_START_ADDR + s_received_size;
        if ((destination < BOOT_APP1_START_ADDR) ||
            (destination >= BOOT_APP1_END_ADDR) ||
            (data_len > (BOOT_APP1_END_ADDR - destination)))
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
        destination_end = destination + data_len;
        if (destination_end <= destination)
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
        last_page = BOOT_APP1_START_ADDR +
                    (((destination_end - 1UL - BOOT_APP1_START_ADDR) /
                      BOOT_FLASH_PAGE_SIZE) * BOOT_FLASH_PAGE_SIZE);
        erase_result = boot_uds_upgrade_erase_app_pages_through(last_page);
        if ((erase_result != UDS_UPGRADE_OK) ||
            !boot_flash_write(destination, data, data_len))
        {
            return (erase_result != UDS_UPGRADE_OK) ?
                   erase_result : (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
        }
    }
    else
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }

    if (s_transfer_type == BOOT_UDS_TRANSFER_APP)
    {
        s_transfer_crc16 = uds_customer_profile_crc16_update(s_transfer_crc16,
                                                              data,
                                                              data_len);
    }
    s_last_bsc = block_counter;
    s_last_len = data_len;
    memcpy(s_last_data, data, data_len);
    s_last_valid = 1U;
    s_received_size += data_len;
    s_expected_bsc++;
    return UDS_UPGRADE_OK;
}

uint8_t uds_upgrade_transfer_exit(void)
{
    if (((s_state != BOOT_UDS_UPGRADE_DRIVER_RX) &&
         (s_state != BOOT_UDS_UPGRADE_APP_RX)) ||
        (s_received_size != s_transfer_size))
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }
    s_state = (s_transfer_type == BOOT_UDS_TRANSFER_DRIVER) ?
              BOOT_UDS_UPGRADE_DRIVER_EXITED : BOOT_UDS_UPGRADE_APP_EXITED;
    return UDS_UPGRADE_OK;
}

uint8_t uds_upgrade_validate_transfer_crc16(uint16_t expected_crc16,
                                             bool_t *crc_matches)
{
    if (crc_matches == 0)
    {
        return (uint8_t)NRC_GENERAL_REJECT;
    }
    *crc_matches = FALSE;

    if (s_state == BOOT_UDS_UPGRADE_DRIVER_EXITED)
    {
        /* A completed FlashDriver transfer is accepted without content/CRC checks. */
        (void)expected_crc16;
        s_driver_received = 1U;
        s_state = BOOT_UDS_UPGRADE_DRIVER_OK;
        *crc_matches = TRUE;
        boot_uds_upgrade_clear_transfer();
        return UDS_UPGRADE_OK;
    }

    if (s_state == BOOT_UDS_UPGRADE_APP_EXITED)
    {
        if (s_transfer_crc16 != expected_crc16)
        {
            s_state = BOOT_UDS_UPGRADE_DRIVER_OK;
            s_next_page_to_erase = BOOT_APP1_START_ADDR;
            s_app_size = 0UL;
            return UDS_UPGRADE_OK;
        }
        s_app_size = s_transfer_size;
        s_expected_crc32 = boot_crc32((const uint8_t *)BOOT_APP1_START_ADDR,
                                      s_app_size);
        s_state = BOOT_UDS_UPGRADE_APP_OK;
        *crc_matches = TRUE;
        boot_uds_upgrade_clear_transfer();
        return UDS_UPGRADE_OK;
    }

    return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
}

uint8_t uds_upgrade_validate_erase_request(uint32_t target_address,
                                            uint32_t image_size)
{
    if (s_state == BOOT_UDS_UPGRADE_APP_ERASED)
    {
        return ((target_address == s_expected_app_address) &&
                (image_size == s_expected_app_size)) ?
               UDS_UPGRADE_OK : (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }
    if ((s_state != BOOT_UDS_UPGRADE_DRIVER_OK) ||
        (s_driver_received == 0U) ||
        (target_address != BOOT_APP1_START_ADDR) ||
        (image_size == 0UL) ||
        (image_size > BOOT_APP_CODE_SIZE))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }
    return UDS_UPGRADE_OK;
}

uint8_t uds_upgrade_erase_application(uint32_t target_address,
                                       uint32_t image_size)
{
    uint32_t first_page;
    uint32_t last_page;
    uint32_t page_address;
    uint8_t result;

    result = uds_upgrade_validate_erase_request(target_address, image_size);
    if (result != UDS_UPGRADE_OK)
    {
        return result;
    }
    if (s_state == BOOT_UDS_UPGRADE_APP_ERASED)
    {
        return UDS_UPGRADE_OK;
    }

    first_page = BOOT_APP1_START_ADDR;
    last_page = BOOT_APP1_START_ADDR +
                (((image_size - 1UL) / BOOT_FLASH_PAGE_SIZE) *
                 BOOT_FLASH_PAGE_SIZE);
    if ((first_page < BOOT_APP1_START_ADDR) ||
        (last_page >= BOOT_APP1_END_ADDR))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }
    for (page_address = s_next_page_to_erase;
         page_address <= last_page;
         page_address += BOOT_FLASH_PAGE_SIZE)
    {
        if (!boot_flash_erase_page(page_address))
        {
            return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
        }
        s_next_page_to_erase = page_address + BOOT_FLASH_PAGE_SIZE;
    }

    s_expected_app_address = target_address;
    s_expected_app_size = image_size;
    s_state = BOOT_UDS_UPGRADE_APP_ERASED;
    return UDS_UPGRADE_OK;
}

uint8_t uds_upgrade_check_programming_dependencies(void)
{
    if ((s_state != BOOT_UDS_UPGRADE_APP_OK) ||
        (s_driver_received == 0U) ||
        (s_fingerprint_written == 0U) ||
        (s_app_size < 8UL) ||
        !boot_app_app1_is_valid())
    {
        return (uint8_t)NRC_CONDITIONS_NOT_CORRECT;
    }
    s_state = BOOT_UDS_UPGRADE_DEPENDENCIES_OK;
    return UDS_UPGRADE_OK;
}

bool_t uds_upgrade_image_verified(void)
{
    return ((s_state == BOOT_UDS_UPGRADE_DEPENDENCIES_OK) ||
            (s_state == BOOT_UDS_UPGRADE_APP1_READY)) ? TRUE : FALSE;
}

bool_t uds_upgrade_mark_app1_ready(void)
{
    if (s_state == BOOT_UDS_UPGRADE_APP1_READY)
    {
        return TRUE;
    }
    if ((s_state != BOOT_UDS_UPGRADE_DEPENDENCIES_OK) ||
        !boot_upgrade_prepare_app1(s_app_size, s_expected_crc32))
    {
        return FALSE;
    }
    s_state = BOOT_UDS_UPGRADE_APP1_READY;
    return TRUE;
}
