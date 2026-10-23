/******************************************************************************
 * Customer UDS download state and Flash integration.
 *
 * 34/36/37 handle one transfer at a time. The first nine-byte transfer must
 * equal ASCII "BMS-DUCAR" and is never executed or programmed. APP download
 * CRC16 covers every transferred package byte, including its DUCAR wrapper
 * and FF alignment. The wrapper's CRC32 covers only the raw APP bytes, which
 * alone are staged in APP2 and later moved by the existing BOOT mechanism.
 ******************************************************************************/
#include "uds_upgrade.h"

#include "App/app_param.h"
#include "App/app_param_api.h"
#include "App/app_flash_map.h"
#include "Driver/d_flash.h"
#include "Protocol/IAP/ducar_uart_iap_protocol.h"
#include "uds_customer_profile.h"

#include <string.h>

typedef enum
{
    UDS_UPGRADE_STATE_IDLE = 0U,
    UDS_UPGRADE_STATE_DRIVER_RECEIVING,
    UDS_UPGRADE_STATE_DRIVER_EXITED,
    UDS_UPGRADE_STATE_DRIVER_VERIFIED,
    UDS_UPGRADE_STATE_APP_ERASED,
    UDS_UPGRADE_STATE_APP_RECEIVING,
    UDS_UPGRADE_STATE_APP_EXITED,
    UDS_UPGRADE_STATE_APP_BLOCK_VERIFIED,
    UDS_UPGRADE_STATE_DEPENDENCIES_VERIFIED,
    UDS_UPGRADE_STATE_HANDOFF
} uds_upgrade_state_t;

typedef enum
{
    UDS_UPGRADE_TRANSFER_NONE = 0U,
    UDS_UPGRADE_TRANSFER_DRIVER,
    UDS_UPGRADE_TRANSFER_APPLICATION
} uds_upgrade_transfer_type_t;

static uds_upgrade_state_t s_state = UDS_UPGRADE_STATE_IDLE;
static uds_upgrade_transfer_type_t s_transfer_type = UDS_UPGRADE_TRANSFER_NONE;
static uint32_t s_target_address;
static uint32_t s_image_size;
static uint32_t s_expected_app_size;
static uint32_t s_expected_package_size;
static uint32_t s_total_image_size;
static uint32_t s_transfer_offset;
static uint32_t s_received_size;
static uint32_t s_expected_crc32;
static uint32_t s_next_page_to_erase;
static uint32_t s_ducar_app_size;
static uint32_t s_ducar_app_crc32;
static uint8_t s_driver_token[UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE];
static uint8_t s_expected_block_counter;
static uint8_t s_last_block_counter;
static uint16_t s_last_block_len;
static uint16_t s_ducar_header_received;
static uint16_t s_transfer_crc16;
static uint8_t s_ducar_header[DUCAR_IAP_PAGE_SIZE];
static uint8_t s_last_block_data[UDS_UPGRADE_BLOCK_DATA_MAX];
static bool_t s_last_block_valid;
static bool_t s_ducar_header_valid;
static bool_t s_fingerprint_written;

#if defined(UDS_UPGRADE_HOST_TEST)
extern uint8_t uds_upgrade_test_app2[APP_FLASH_MAP_APP2_END_ADDR -
                                     APP_FLASH_MAP_APP2_START_ADDR];
#define UDS_UPGRADE_APP2_BYTES uds_upgrade_test_app2
#else
#define UDS_UPGRADE_APP2_BYTES ((uint8_t *)APP_FLASH_MAP_APP2_START_ADDR)
#endif

static void uds_upgrade_clear_transfer(void)
{
    s_transfer_type = UDS_UPGRADE_TRANSFER_NONE;
    s_received_size = 0UL;
    s_expected_block_counter = 1U;
    s_last_block_counter = 0U;
    s_last_block_len = 0U;
    s_ducar_header_received = 0U;
    s_transfer_crc16 = 0xFFFFU;
    s_ducar_app_size = 0UL;
    s_ducar_app_crc32 = 0UL;
    s_last_block_valid = FALSE;
    s_ducar_header_valid = FALSE;
    memset(s_ducar_header, 0xFF, sizeof(s_ducar_header));
    memset(s_last_block_data, 0, sizeof(s_last_block_data));
}

static void uds_upgrade_begin_transfer(uds_upgrade_transfer_type_t type,
                                       uint32_t target_address,
                                       uint32_t image_size)
{
    s_transfer_type = type;
    s_target_address = target_address;
    s_image_size = image_size;
    s_transfer_offset = (type == UDS_UPGRADE_TRANSFER_APPLICATION) ?
                        s_total_image_size : 0UL;
    s_received_size = 0UL;
    s_expected_block_counter = 1U;
    s_last_block_counter = 0U;
    s_last_block_len = 0U;
    s_ducar_header_received = 0U;
    s_transfer_crc16 = 0xFFFFU;
    s_ducar_app_size = 0UL;
    s_ducar_app_crc32 = 0UL;
    s_last_block_valid = FALSE;
    s_ducar_header_valid = FALSE;
    memset(s_ducar_header, 0xFF, sizeof(s_ducar_header));
    memset(s_last_block_data, 0, sizeof(s_last_block_data));
}

static uint32_t uds_upgrade_read_le32(const uint8_t *data)
{
    return ((uint32_t)data[0] |
            ((uint32_t)data[1] << 8) |
            ((uint32_t)data[2] << 16) |
            ((uint32_t)data[3] << 24));
}

static uint32_t uds_upgrade_align_up_128(uint32_t value)
{
    if (value > 0xFFFFFF80UL)
    {
        return 0UL;
    }
    return ((value + 127UL) & ~127UL);
}

/* Parse the DUCAR tool's 128-byte wrapper; it is metadata, never APP code. */
static bool_t uds_upgrade_parse_ducar_header(uint32_t package_size)
{
    static const uint8_t expected_mark[DUCAR_IAP_META_MARK_LEN] =
        {'D', 'U', 'C', 'A', 'R', 0xFFU, 0xFFU};
    char firmware_part_number[DUCAR_IAP_META_PART_LEN + 1U];
    uint32_t image_size;
    uint32_t padded_image_size;
    uint32_t expected_package_size;

    if (memcmp(&s_ducar_header[DUCAR_IAP_META_MARK_OFFSET],
               expected_mark,
               sizeof(expected_mark)) != 0)
    {
        return FALSE;
    }

    memcpy(firmware_part_number,
           &s_ducar_header[DUCAR_IAP_META_PART_OFFSET],
           DUCAR_IAP_META_PART_LEN);
    firmware_part_number[DUCAR_IAP_META_PART_LEN] = '\0';
    if (!ducar_iap_part_number_is_allowed(app_param_get_software_pn(),
                                          firmware_part_number))
    {
        return FALSE;
    }

    image_size = uds_upgrade_read_le32(&s_ducar_header[DUCAR_IAP_META_SIZE_OFFSET]);
    padded_image_size = uds_upgrade_align_up_128(image_size);
    if ((image_size == 0UL) || (padded_image_size == 0UL) ||
        (image_size != s_expected_app_size) ||
        (image_size > APP_FLASH_MAP_APP_CODE_SIZE) ||
        (image_size > (APP_FLASH_MAP_APP2_END_ADDR -
                       APP_FLASH_MAP_APP2_START_ADDR - s_transfer_offset)) ||
        (padded_image_size > (0xFFFFFFFFUL - DUCAR_IAP_PAGE_SIZE)))
    {
        return FALSE;
    }

    expected_package_size = (uint32_t)DUCAR_IAP_PAGE_SIZE + padded_image_size;
    if ((package_size != expected_package_size) ||
        (package_size != s_expected_package_size))
    {
        return FALSE;
    }

    s_ducar_app_size = image_size;
    s_ducar_app_crc32 = uds_upgrade_read_le32(&s_ducar_header[DUCAR_IAP_META_CRC_OFFSET]);
    s_ducar_header_valid = TRUE;
    return TRUE;
}

/* Any failed APP package must be erased from the beginning before a retry. */
static void uds_upgrade_abort_application(void)
{
    s_total_image_size = 0UL;
    s_target_address = APP_FLASH_MAP_APP1_START_ADDR;
    s_expected_app_size = 0UL;
    s_expected_package_size = 0UL;
    s_next_page_to_erase = APP_FLASH_MAP_APP2_START_ADDR;
    s_state = UDS_UPGRADE_STATE_DRIVER_VERIFIED;
    uds_upgrade_clear_transfer();
}

/* Stream the wrapper, then program only raw APP bytes and verify FF alignment. */
static uint8_t uds_upgrade_store_application_block(const uint8_t *data,
                                                   uint16_t data_len)
{
    uint32_t stream_offset = s_received_size;
    uint32_t header_bytes;
    uint32_t payload_offset;
    uint32_t payload_bytes;
    uint32_t padding_offset;
    uint32_t index;
    uint32_t destination;
    uint16_t consumed;

    consumed = 0U;
    if (stream_offset < DUCAR_IAP_PAGE_SIZE)
    {
        header_bytes = DUCAR_IAP_PAGE_SIZE - stream_offset;
        if (header_bytes > data_len)
        {
            header_bytes = data_len;
        }
        memcpy(&s_ducar_header[stream_offset], data, header_bytes);
        consumed = (uint16_t)header_bytes;
        stream_offset += header_bytes;
        if (stream_offset > s_ducar_header_received)
        {
            s_ducar_header_received = (uint16_t)stream_offset;
        }

        if ((s_ducar_header_received == DUCAR_IAP_PAGE_SIZE) &&
            !s_ducar_header_valid &&
            !uds_upgrade_parse_ducar_header(s_image_size))
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
    }

    if (consumed == data_len)
    {
        return UDS_UPGRADE_OK;
    }
    if (!s_ducar_header_valid || (stream_offset < DUCAR_IAP_PAGE_SIZE))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    payload_offset = stream_offset - DUCAR_IAP_PAGE_SIZE;
    payload_bytes = (uint32_t)data_len - consumed;
    if (payload_offset < s_ducar_app_size)
    {
        uint32_t remaining_image_bytes = s_ducar_app_size - payload_offset;
        if (payload_bytes > remaining_image_bytes)
        {
            payload_bytes = remaining_image_bytes;
        }
    }
    else
    {
        payload_bytes = 0UL;
    }

    padding_offset = (uint32_t)consumed + payload_bytes;
    for (index = padding_offset; index < data_len; ++index)
    {
        if (data[index] != 0xFFU)
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }
    }

    if (payload_bytes > 0UL)
    {
        destination = APP_FLASH_MAP_APP2_START_ADDR +
                      s_transfer_offset + payload_offset;
        if (!d_flash_write(destination, &data[consumed], payload_bytes))
        {
            return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
        }
    }

    return UDS_UPGRADE_OK;
}

/* Reset volatile protocol state only; keep the persistent fingerprint and APP2. */
void uds_upgrade_reset_state(void)
{
    s_state = UDS_UPGRADE_STATE_IDLE;
    s_target_address = 0UL;
    s_image_size = 0UL;
    s_expected_app_size = 0UL;
    s_expected_package_size = 0UL;
    s_total_image_size = 0UL;
    s_transfer_offset = 0UL;
    s_expected_crc32 = 0UL;
    s_next_page_to_erase = APP_FLASH_MAP_APP2_START_ADDR;
    s_fingerprint_written = FALSE;
    memset(s_driver_token, 0, sizeof(s_driver_token));
    uds_upgrade_clear_transfer();
}

/* Persist the exact customer eight-byte fingerprint behind the 16-bit FP tag. */
uint8_t uds_upgrade_store_fingerprint(const uint8_t *data, uint16_t length)
{
    uint8_t record[2U + UDS_CUSTOMER_FINGERPRINT_SIZE];

    s_fingerprint_written = FALSE;
    if (!uds_customer_profile_validate_fingerprint(data, length))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    record[0] = (uint8_t)(APP_FLASH_TAG_UDS_DOWNLOAD_FINGERPRINT & 0x00FFU);
    record[1] = (uint8_t)(APP_FLASH_TAG_UDS_DOWNLOAD_FINGERPRINT >> 8);
    memcpy(&record[2], data, UDS_CUSTOMER_FINGERPRINT_SIZE);

    if (!d_flash_erase_page(APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR) ||
        !d_flash_write(APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR,
                       record,
                       sizeof(record)))
    {
        return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
    }

    s_fingerprint_written = TRUE;
    return UDS_UPGRADE_OK;
}

/* This fixed-size download is the BMS-DUCAR token, not executable code. */
uint8_t uds_upgrade_request_download(uint32_t target_address, uint32_t download_size)
{
    if (s_state == UDS_UPGRADE_STATE_IDLE)
    {
        if (!s_fingerprint_written)
        {
            return (uint8_t)NRC_CONDITIONS_NOT_CORRECT;
        }
        if (download_size != UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE)
        {
            return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
        }

        uds_upgrade_begin_transfer(UDS_UPGRADE_TRANSFER_DRIVER,
                                   target_address,
                                   download_size);
        s_state = UDS_UPGRADE_STATE_DRIVER_RECEIVING;
        return UDS_UPGRADE_OK;
    }

    if ((s_state == UDS_UPGRADE_STATE_APP_ERASED) &&
        (target_address == s_target_address) &&
        (download_size == s_expected_package_size) &&
        (download_size != UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE))
    {
        uds_upgrade_begin_transfer(UDS_UPGRADE_TRANSFER_APPLICATION,
                                   target_address,
                                   download_size);
        s_state = UDS_UPGRADE_STATE_APP_RECEIVING;
        return UDS_UPGRADE_OK;
    }

    return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
}

/* Verify that a resent last block is byte-identical, without programming twice. */
static bool_t uds_upgrade_duplicate_matches(uint8_t block_counter,
                                           const uint8_t *data,
                                           uint16_t data_len)
{
    const uint8_t *previous_data;

    if ((!s_last_block_valid) || (block_counter != s_last_block_counter) ||
        (data_len != s_last_block_len))
    {
        return FALSE;
    }

    if (s_transfer_type == UDS_UPGRADE_TRANSFER_DRIVER)
    {
        previous_data = s_driver_token;
    }
    else
    {
        previous_data = s_last_block_data;
    }
    return (memcmp(data, previous_data, data_len) == 0) ? TRUE : FALSE;
}

/* Store one real data block; the BSC itself is never included in image bytes. */
uint8_t uds_upgrade_transfer_data(uint8_t block_counter,
                                  const uint8_t *data,
                                  uint16_t data_len)
{
    uint32_t remaining;
    uint16_t expected_len;
    uint8_t result;

    if (((s_state != UDS_UPGRADE_STATE_DRIVER_RECEIVING) &&
         (s_state != UDS_UPGRADE_STATE_APP_RECEIVING)) ||
        (data == NULL) || (data_len == 0U) ||
        (data_len > UDS_UPGRADE_BLOCK_DATA_MAX))
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }

    if (s_last_block_valid && (block_counter == s_last_block_counter))
    {
        return uds_upgrade_duplicate_matches(block_counter, data, data_len) ?
               UDS_UPGRADE_OK : (uint8_t)NRC_WRONG_BLOCK_SEQUENCE_COUNTER;
    }
    if (block_counter != s_expected_block_counter)
    {
        return (uint8_t)NRC_WRONG_BLOCK_SEQUENCE_COUNTER;
    }

    if (s_received_size > s_image_size)
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }
    remaining = s_image_size - s_received_size;
    expected_len = (uint16_t)((remaining > UDS_UPGRADE_BLOCK_DATA_MAX) ?
                              UDS_UPGRADE_BLOCK_DATA_MAX : remaining);
    if (data_len != expected_len)
    {
        return (uint8_t)NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT;
    }

    if (s_transfer_type == UDS_UPGRADE_TRANSFER_DRIVER)
    {
        if ((s_received_size != 0UL) ||
            (data_len != UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE))
        {
            return (uint8_t)NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT;
        }
        memcpy(s_driver_token, data, UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE);
    }
    else if (s_transfer_type == UDS_UPGRADE_TRANSFER_APPLICATION)
    {
        result = uds_upgrade_store_application_block(data, data_len);
        if (result != UDS_UPGRADE_OK)
        {
            uds_upgrade_abort_application();
            return result;
        }
    }
    else
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }

    s_last_block_counter = block_counter;
    s_last_block_len = data_len;
    memcpy(s_last_block_data, data, data_len);
    s_last_block_valid = TRUE;
    s_transfer_crc16 = uds_customer_profile_crc16_update(s_transfer_crc16,
                                                          data,
                                                          data_len);
    s_received_size += (uint32_t)data_len;
    s_expected_block_counter++;
    return UDS_UPGRADE_OK;
}

/* 37 closes data reception only; CRC16 is a separate 31 01 02 02 routine. */
uint8_t uds_upgrade_transfer_exit(void)
{
    if (((s_state != UDS_UPGRADE_STATE_DRIVER_RECEIVING) &&
         (s_state != UDS_UPGRADE_STATE_APP_RECEIVING)) ||
        (s_received_size != s_image_size))
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }

    s_state = (s_transfer_type == UDS_UPGRADE_TRANSFER_DRIVER) ?
              UDS_UPGRADE_STATE_DRIVER_EXITED : UDS_UPGRADE_STATE_APP_EXITED;
    return UDS_UPGRADE_OK;
}

/* Compare tester CRC16 with the transfer stream: token or full wrapped APP BIN. */
uint8_t uds_upgrade_validate_transfer_crc16(uint16_t expected_crc16,
                                             bool_t *crc_matches)
{
    uint16_t actual_crc16;

    if (crc_matches == NULL)
    {
        return (uint8_t)NRC_GENERAL_REJECT;
    }
    *crc_matches = FALSE;

    if (s_state == UDS_UPGRADE_STATE_DRIVER_EXITED)
    {
        actual_crc16 = uds_customer_profile_crc16(s_driver_token,
                                                   UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE);
        if (actual_crc16 == expected_crc16)
        {
            if (!uds_customer_profile_is_flash_driver(s_driver_token,
                                                       UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE))
            {
                s_state = UDS_UPGRADE_STATE_IDLE;
                uds_upgrade_clear_transfer();
                return UDS_UPGRADE_OK;
            }
            s_state = UDS_UPGRADE_STATE_DRIVER_VERIFIED;
            *crc_matches = TRUE;
        }
        else
        {
            s_state = UDS_UPGRADE_STATE_IDLE;
            memset(s_driver_token, 0, sizeof(s_driver_token));
            uds_upgrade_clear_transfer();
        }
        return UDS_UPGRADE_OK;
    }

    if (s_state == UDS_UPGRADE_STATE_APP_EXITED)
    {
        actual_crc16 = s_transfer_crc16;
        if ((actual_crc16 == expected_crc16) &&
            s_ducar_header_valid &&
            (s_ducar_header_received == DUCAR_IAP_PAGE_SIZE) &&
            (s_ducar_app_size == s_expected_app_size) &&
            (ducar_iap_crc32_calc(
                 &UDS_UPGRADE_APP2_BYTES[s_transfer_offset],
                 s_ducar_app_size) == s_ducar_app_crc32))
        {
            s_total_image_size += s_ducar_app_size;
            s_state = UDS_UPGRADE_STATE_APP_BLOCK_VERIFIED;
            *crc_matches = TRUE;
            uds_upgrade_clear_transfer();
        }
        else
        {
            uds_upgrade_abort_application();
        }
        return UDS_UPGRADE_OK;
    }

    return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
}

/* 31 FF00 declares the only supported logical code range (APP1 -> APP2 staging). */
uint8_t uds_upgrade_validate_erase_request(uint32_t target_address,
                                            uint32_t image_size)
{
    uint32_t expected_address;

    if ((s_state != UDS_UPGRADE_STATE_DRIVER_VERIFIED) &&
        (s_state != UDS_UPGRADE_STATE_APP_BLOCK_VERIFIED))
    {
        return (uint8_t)NRC_REQUEST_SEQUENCE_ERROR;
    }
    expected_address = APP_FLASH_MAP_APP1_START_ADDR + s_total_image_size;
    if ((target_address != expected_address) || (image_size == 0UL) ||
        (s_total_image_size > APP_FLASH_MAP_APP_CODE_SIZE) ||
        (image_size > (APP_FLASH_MAP_APP_CODE_SIZE - s_total_image_size)) ||
        (image_size > (APP_FLASH_MAP_APP2_END_ADDR -
                       APP_FLASH_MAP_APP2_START_ADDR - s_total_image_size)) ||
        ((s_total_image_size != 0UL) && ((s_total_image_size & 0x03UL) != 0UL)))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }
    return UDS_UPGRADE_OK;
}

/* Erase only APP2 pages covered by the declared application image. */
uint8_t uds_upgrade_erase_application(uint32_t target_address,
                                       uint32_t image_size)
{
    uint32_t first_page;
    uint32_t last_page;
    uint32_t page_address;
    uint8_t validation;

    validation = uds_upgrade_validate_erase_request(target_address, image_size);
    if (validation != UDS_UPGRADE_OK)
    {
        /* An identical retry after a lost routine response is safely idempotent. */
        if ((s_state == UDS_UPGRADE_STATE_APP_ERASED) &&
            (s_target_address == target_address) &&
            (s_expected_app_size == image_size))
        {
            return UDS_UPGRADE_OK;
        }
        return validation;
    }

    first_page = APP_FLASH_MAP_APP2_START_ADDR +
                 ((s_total_image_size / D_FLASH_PAGE_SIZE) * D_FLASH_PAGE_SIZE);
    last_page = APP_FLASH_MAP_APP2_START_ADDR +
                (((s_total_image_size + image_size - 1UL) / D_FLASH_PAGE_SIZE) *
                 D_FLASH_PAGE_SIZE);
    if ((first_page < APP_FLASH_MAP_APP2_START_ADDR) ||
        (last_page >= APP_FLASH_MAP_APP2_END_ADDR))
    {
        return (uint8_t)NRC_REQUEST_OUT_OF_RANGE;
    }

    /* Keep earlier contiguous logical blocks when they share the last page. */
    for (page_address = s_next_page_to_erase;
         page_address <= last_page;
         page_address += D_FLASH_PAGE_SIZE)
    {
        if (!d_flash_erase_page(page_address))
        {
            return (uint8_t)NRC_GENERAL_PROGRAMMING_FAILURE;
        }
        s_next_page_to_erase = page_address + D_FLASH_PAGE_SIZE;
    }

    s_target_address = target_address;
    s_image_size = 0UL;
    s_expected_app_size = image_size;
    s_expected_package_size = (uint32_t)DUCAR_IAP_PAGE_SIZE +
                              uds_upgrade_align_up_128(image_size);
    s_expected_crc32 = 0UL;
    uds_upgrade_clear_transfer();
    s_state = UDS_UPGRADE_STATE_APP_ERASED;
    return UDS_UPGRADE_OK;
}

/* FF01 represents this APP's dependency gate: marker, APP CRC and fingerprint. */
uint8_t uds_upgrade_check_programming_dependencies(void)
{
    if ((s_state != UDS_UPGRADE_STATE_APP_BLOCK_VERIFIED) ||
        (s_total_image_size < 8UL) || !s_fingerprint_written ||
        !uds_customer_profile_is_flash_driver(s_driver_token,
                                               UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE))
    {
        return (uint8_t)NRC_CONDITIONS_NOT_CORRECT;
    }

    s_expected_crc32 = ducar_iap_crc32_calc(UDS_UPGRADE_APP2_BYTES,
                                             s_total_image_size);
    s_state = UDS_UPGRADE_STATE_DEPENDENCIES_VERIFIED;
    /* The fixed token has served its only purpose; it is not an executable driver. */
    memset(s_driver_token, 0, sizeof(s_driver_token));
    return UDS_UPGRADE_OK;
}

bool_t uds_upgrade_image_verified(void)
{
    return ((s_state == UDS_UPGRADE_STATE_DEPENDENCIES_VERIFIED) ||
            (s_state == UDS_UPGRADE_STATE_HANDOFF)) ? TRUE : FALSE;
}

/* Arm the existing BOOT move metadata only after customer FF01 and before 11 03. */
bool_t uds_upgrade_prepare_boot_move(void)
{
    if (s_state == UDS_UPGRADE_STATE_HANDOFF)
    {
        return TRUE;
    }
    if (s_state != UDS_UPGRADE_STATE_DEPENDENCIES_VERIFIED)
    {
        return FALSE;
    }
    if (!ducar_iap_prepare_boot_move(s_total_image_size, s_expected_crc32))
    {
        return FALSE;
    }

    s_state = UDS_UPGRADE_STATE_HANDOFF;
    return TRUE;
}
