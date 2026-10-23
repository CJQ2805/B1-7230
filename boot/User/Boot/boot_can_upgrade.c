/*
 * BOOT侧DUCAR CAN升级状态机。
 * BOOT直接接收E1控制和DF00 TP页数据，不依赖APP搬运；首个页面先解析升级头，
 * 后续页面按地址连续写入APP2，收齐整包后校验CRC，最后准备搬运并复位进入APP。
 */
#include "Boot/boot_can_upgrade.h"

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Boot/boot_upgrade.h"
#include "Hal_Driver/boot_flash.h"
#include "Hal_Driver/boot_timer.h"
#include "Hal_Driver/boot_watchdog.h"
#include "Hal_Driver/boot_can.h"
#include "Protocol/ducar_iap_protocol.h"
#include "n32g45x.h"
#include <string.h>

#define BOOT_CAN_UPGRADE_PRIORITY       ((uint8_t)6U)
#define BOOT_CAN_UPGRADE_PGN            ((uint16_t)0xDF00U)
#define BOOT_CAN_PAGE_SIZE              ((uint32_t)128UL)
#define BOOT_CAN_PAGE_MESSAGE_LEN       ((uint16_t)132U)
#define BOOT_CAN_PAGE_PACKET_COUNT      ((uint8_t)19U)
#define BOOT_CAN_PACKAGE_HEADER_SIZE    ((uint32_t)128UL)
#define BOOT_CAN_TIMEOUT_MS             ((uint32_t)500UL)
#define BOOT_CAN_FINISH_TIMEOUT_MS      ((uint32_t)1000UL)
#define BOOT_CAN_VERSION_T              ((uint8_t)0xFDU)
#define BOOT_CAN_VERSION_V              ((uint8_t)0xFEU)
#define BOOT_CAN_VERSION_PROTOCOL       ((uint8_t)0xFFU)

typedef enum
{
    BOOT_CAN_STATE_IDLE = 0U,
    BOOT_CAN_STATE_WAIT_PAGE,
    BOOT_CAN_STATE_RECEIVING_PAGE,
    BOOT_CAN_STATE_FINISH_PENDING
} boot_can_state_t;

static boot_can_upgrade_tx_callback_t s_boot_can_tx;
static boot_can_state_t s_boot_can_state;
static uint32_t s_boot_can_firmware_size;
static uint32_t s_boot_can_package_size;
static uint32_t s_boot_can_package_received;
static uint32_t s_boot_can_firmware_received;
static ducar_iap_firmware_info_t s_boot_can_fw_info;
static uint8_t s_boot_can_header_received;
static uint16_t s_boot_can_page_length;
static uint8_t s_boot_can_page_packets;
static uint8_t s_boot_can_expected_packet;
static uint8_t s_boot_can_host_address;
static uint32_t s_boot_can_last_activity;
static uint8_t s_boot_can_reset_pending;
static uint32_t s_boot_can_tx_failure_baseline;
static uint8_t s_boot_can_page_buffer[BOOT_CAN_PAGE_MESSAGE_LEN];

/* 计算128字节升级头加按128字节对齐后的固件区总长度。 */
static uint32_t boot_can_package_size(uint32_t firmware_size)
{
    uint32_t u32DataPageCount = (firmware_size + BOOT_CAN_PAGE_SIZE - 1UL) / BOOT_CAN_PAGE_SIZE;
    return BOOT_CAN_PACKAGE_HEADER_SIZE + u32DataPageCount * BOOT_CAN_PAGE_SIZE;
}

/* 校验报文是否来自PC；direct用于区分点对点报文和允许全局地址的E1报文。 */
static uint8_t boot_can_is_host(const ducar_can_id_fields_t *fields, uint8_t direct)
{
    if ((fields == 0) || (fields->sa != DUCAR_CAN_PC_ADDRESS))
    {
        return 0U;
    }
    if (direct != 0U)
    {
        return (fields->ps == DUCAR_CAN_BMS_A_ADDRESS) ? 1U : 0U;
    }
    return ((fields->ps == DUCAR_CAN_BMS_A_ADDRESS) ||
            (fields->ps == DUCAR_CAN_GLOBAL_ADDRESS)) ? 1U : 0U;
}

/* 清除当前BOOT升级会话及页缓存，回到等待E1/02的初始状态。 */
static void boot_can_reset_state(void)
{
    s_boot_can_state = BOOT_CAN_STATE_IDLE;
    s_boot_can_firmware_size = 0UL;
    s_boot_can_package_size = 0UL;
    s_boot_can_package_received = 0UL;
    s_boot_can_firmware_received = 0UL;
    memset(&s_boot_can_fw_info, 0, sizeof(s_boot_can_fw_info));
    s_boot_can_header_received = 0U;
    s_boot_can_page_length = 0U;
    s_boot_can_page_packets = 0U;
    s_boot_can_expected_packet = 0U;
    s_boot_can_host_address = DUCAR_CAN_PC_ADDRESS;
    s_boot_can_last_activity = 0UL;
    s_boot_can_reset_pending = 0U;
    s_boot_can_tx_failure_baseline = 0UL;
    memset(s_boot_can_page_buffer, 0xFF, sizeof(s_boot_can_page_buffer));
}

/* 通过底层CAN回调发送BOOT协议帧。 */
static uint8_t boot_can_send(const ducar_can_frame_t *frame)
{
    return ((s_boot_can_tx != 0) && (frame != 0) && s_boot_can_tx(frame)) ? 1U : 0U;
}

/* 初始化BOOT发送帧，统一使用BMS源地址和升级优先级。 */
static void boot_can_frame_init(ducar_can_frame_t *frame, uint8_t pf, uint8_t destination)
{
    ducar_can_frame_init(frame,
        ducar_can_build_id(BOOT_CAN_UPGRADE_PRIORITY, 0U, 0U, pf,
                           destination, DUCAR_CAN_BMS_A_ADDRESS));
}

/* 回复E1控制结果，并在Data1标识当前响应来自BOOT区域。 */
static uint8_t boot_can_send_e1_response(uint8_t destination, uint8_t status)
{
    ducar_can_frame_t tCanFrame;

    boot_can_frame_init(&tCanFrame, DUCAR_CAN_PF_ACK, destination);
    tCanFrame.data[0] = status;
    tCanFrame.data[1] = DUCAR_IAP_IN_BOOT;
    tCanFrame.data[2] = 0xFFU;
    tCanFrame.data[3] = 0xFFU;
    tCanFrame.data[4] = 0xFFU;
    tCanFrame.data[5] = 0x00U;
    tCanFrame.data[6] = DUCAR_CAN_PF_BOOT_CONTROL;
    tCanFrame.data[7] = 0x00U;
    return boot_can_send(&tCanFrame);
}

/* 对主机RTS回复CTS，允许本页19个DT包从序号1开始发送。 */
static uint8_t boot_can_send_cts(uint8_t destination)
{
    ducar_can_frame_t tCanFrame;

    boot_can_frame_init(&tCanFrame, DUCAR_CAN_PF_TP_CM, destination);
    tCanFrame.data[0] = 0x11U;
    tCanFrame.data[1] = BOOT_CAN_PAGE_PACKET_COUNT;
    tCanFrame.data[2] = 0x01U;
    tCanFrame.data[3] = 0xFFU;
    tCanFrame.data[4] = 0xFFU;
    tCanFrame.data[5] = 0x00U;
    tCanFrame.data[6] = (uint8_t)(BOOT_CAN_UPGRADE_PGN >> 8);
    tCanFrame.data[7] = 0x00U;
    return boot_can_send(&tCanFrame);
}

/* 一页写入完成后回复EOM，通知主机当前页已接收。 */
static uint8_t boot_can_send_eom(uint8_t destination)
{
    ducar_can_frame_t tCanFrame;

    boot_can_frame_init(&tCanFrame, DUCAR_CAN_PF_TP_CM, destination);
    tCanFrame.data[0] = 0x13U;
    tCanFrame.data[1] = (uint8_t)(s_boot_can_page_length & 0xFFU);
    tCanFrame.data[2] = (uint8_t)(s_boot_can_page_length >> 8);
    tCanFrame.data[3] = s_boot_can_page_packets;
    tCanFrame.data[4] = 0xFFU;
    tCanFrame.data[5] = 0x00U;
    tCanFrame.data[6] = (uint8_t)(BOOT_CAN_UPGRADE_PGN >> 8);
    tCanFrame.data[7] = 0x00U;
    return boot_can_send(&tCanFrame);
}

/* 页格式、页序、Flash或超时异常时终止当前TP会话。 */
static uint8_t boot_can_send_abort(uint8_t destination)
{
    ducar_can_frame_t tCanFrame;

    boot_can_frame_init(&tCanFrame, DUCAR_CAN_PF_TP_CM, destination);
    tCanFrame.data[0] = 0xFFU;
    tCanFrame.data[1] = 0xFFU;
    tCanFrame.data[2] = 0xFFU;
    tCanFrame.data[3] = 0xFFU;
    tCanFrame.data[4] = 0xFFU;
    tCanFrame.data[5] = 0x00U;
    tCanFrame.data[6] = (uint8_t)(BOOT_CAN_UPGRADE_PGN >> 8);
    tCanFrame.data[7] = 0x00U;
    return boot_can_send(&tCanFrame);
}

/*
 * 回复F200版本查询。
 * BOOT不读取APP版本参数，避免BOOT处于独立升级状态时依赖APP数据；
 * 按当前协议约定，BOOT固定返回FD FE FF，剩余字节保持0xFF。
 */
static uint8_t boot_can_send_version_response(void)
{
    ducar_can_frame_t tCanFrame;

    /* F200是版本数据PGN，响应使用PF=F2、PS=00、SA=BMS_A。 */
    boot_can_frame_init(&tCanFrame,
                        (uint8_t)(DUCAR_CAN_PGN_F200 >> 8U),
                        (uint8_t)DUCAR_CAN_PGN_F200);
    /* Data0固定为BOOT版本T字段FD。 */
    tCanFrame.data[0] = BOOT_CAN_VERSION_T;
    /* Data1固定为BOOT版本V字段FE。 */
    tCanFrame.data[1] = BOOT_CAN_VERSION_V;
    /* Data2固定为协议/版本标识FF。 */
    tCanFrame.data[2] = BOOT_CAN_VERSION_PROTOCOL;
    /* Data3~Data7由ducar_can_frame_init保持为0xFF。 */
    return boot_can_send(&tCanFrame);
}

/* 判断报文是否为PC发起的F200版本查询。 */
static uint8_t boot_can_is_version_request(const ducar_can_frame_t *frame,
                                           const ducar_can_id_fields_t *fields)
{
    /* 查询必须包含PGN低字节、高字节和有效控制字节。 */
    if ((frame == 0) || (fields == 0) || (frame->dlc < 3U))
    {
        return 0U;
    }

    /* 版本查询只能由PC发起，并且目标必须是本BMS或全局地址。 */
    if (boot_can_is_host(fields, 0U) == 0U)
    {
        return 0U;
    }

    /* Data0/Data1组成F200，Data2为0表示有效查询。 */
    return ((frame->data[0] == (uint8_t)DUCAR_CAN_PGN_F200) &&
            (frame->data[1] == (uint8_t)(DUCAR_CAN_PGN_F200 >> 8U)) &&
            (frame->data[2] == 0U)) ? 1U : 0U;
}

/* 首页升级头验证通过后擦除BOOT侧APP2暂存区。 */
static bool boot_can_erase_app2(void)
{
    uint32_t u32FlashAddress;

    for (u32FlashAddress = BOOT_APP2_START_ADDR; u32FlashAddress < BOOT_APP2_END_ADDR;
         u32FlashAddress += BOOT_FLASH_PAGE_SIZE)
    {
        boot_watchdog_feed();
        if (!boot_flash_erase_page(u32FlashAddress))
        {
            return false;
        }
    }
    return true;
}

/* 从首页128字节头中解析固件大小、CRC和型号，并在此之后才允许擦APP2。 */
static bool boot_can_parse_header(void)
{
    ducar_iap_frame_t tIapFrame;
    ducar_iap_firmware_info_t tFirmwareInfo;
    uint32_t u32PackageSize;

    memset(&tIapFrame, 0, sizeof(tIapFrame));
    tIapFrame.cmd = DUCAR_IAP_CMD_PROGRAM;
    tIapFrame.len = DUCAR_IAP_PROGRAM_DATA_LEN;
    memcpy(&tIapFrame.data[2], &s_boot_can_page_buffer[4],
           BOOT_CAN_PACKAGE_HEADER_SIZE);
    if (!ducar_iap_parse_firmware_info(&tIapFrame, &tFirmwareInfo))
    {
        return false;
    }
    if ((!ducar_iap_part_number_is_allowed(BOOT_PARAM_SOFTWARE_PN,
                                          tFirmwareInfo.part_number)) ||
        (tFirmwareInfo.size < 8UL) ||
        (tFirmwareInfo.size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    u32PackageSize = boot_can_package_size(tFirmwareInfo.size);
    if ((u32PackageSize <= BOOT_CAN_PACKAGE_HEADER_SIZE) ||
        (u32PackageSize > (BOOT_CAN_PACKAGE_HEADER_SIZE +
                           BOOT_APP_CODE_SIZE + BOOT_CAN_PAGE_SIZE - 1UL)))
    {
        return false;
    }

    /* 首页面头和长度合法后才擦APP2；E1/02阶段绝不擦除。 */
    if (!boot_can_erase_app2())
    {
        return false;
    }

    s_boot_can_fw_info = tFirmwareInfo;
    s_boot_can_firmware_size = tFirmwareInfo.size;
    s_boot_can_package_size = u32PackageSize;
    s_boot_can_header_received = 1U;
    return true;
}

/* 对APP2中的完整固件计算CRC32并与升级头中的CRC比较。 */
static bool boot_can_crc_is_valid(void)
{
    return (s_boot_can_header_received != 0U) &&
           (ducar_iap_crc32_calc((const uint8_t *)BOOT_APP2_START_ADDR,
                                 s_boot_can_fw_info.size) == s_boot_can_fw_info.crc32);
}

/* 检查主机页地址是否连续、按128字节对齐且未超出本次升级包。 */
static bool boot_can_page_address_valid(uint32_t address, uint32_t *offset)
{
    uint32_t u32PageOffset;

    if (offset == 0)
    {
        return false;
    }
    if ((address >= BOOT_APP1_START_ADDR) &&
        (address < (BOOT_APP1_START_ADDR + s_boot_can_package_size)))
    {
        u32PageOffset = address - BOOT_APP1_START_ADDR;
    }
    else
    {
        u32PageOffset = address;
    }
    if ((u32PageOffset != s_boot_can_package_received) ||
        ((u32PageOffset % BOOT_CAN_PAGE_SIZE) != 0UL) ||
        (u32PageOffset >= s_boot_can_package_size))
    {
        return false;
    }
    *offset = u32PageOffset;
    return true;
}

/* 将收齐的132字节页写入APP2；首页只解析头，后续页写入固件数据。 */
static bool boot_can_write_page(void)
{
    uint32_t u32PageAddress;
    uint32_t u32PageOffset;
    uint32_t u32FirmwareBytesRemaining;
    uint32_t u32WriteLength;

    u32PageAddress = ducar_can_read_u32_le(s_boot_can_page_buffer);
    if (s_boot_can_header_received == 0U)
    {
        /* 首页只能是偏移0或APP1绝对首地址；长度尚未可信，不用它计算范围。 */
        if ((u32PageAddress != 0UL) &&
            (u32PageAddress != BOOT_APP1_START_ADDR))
        {
            return false;
        }

        if (!boot_can_parse_header())
        {
            return false;
        }

        s_boot_can_package_received += BOOT_CAN_PAGE_SIZE;
        return true;
    }

    if (!boot_can_page_address_valid(u32PageAddress, &u32PageOffset))
    {
        return false;
    }
    if (u32PageOffset < BOOT_CAN_PACKAGE_HEADER_SIZE)
    {
        return false;
    }

    u32PageOffset -= BOOT_CAN_PACKAGE_HEADER_SIZE;
    if (u32PageOffset >= s_boot_can_firmware_size)
    {
        return false;
    }

    u32FirmwareBytesRemaining = s_boot_can_firmware_size - u32PageOffset;
    u32WriteLength = (u32FirmwareBytesRemaining > BOOT_CAN_PAGE_SIZE) ?
                     BOOT_CAN_PAGE_SIZE : u32FirmwareBytesRemaining;
    if (!boot_flash_write(BOOT_APP2_START_ADDR + u32PageOffset,
                          &s_boot_can_page_buffer[4],
                          u32WriteLength))
    {
        return false;
    }

    s_boot_can_firmware_received += u32WriteLength;
    s_boot_can_package_received += BOOT_CAN_PAGE_SIZE;
    return true;
}

/* 处理BOOT收到的E1/01查询、E1/02开始接收和E1/03完成确认。 */
static void boot_can_handle_e1(const ducar_can_frame_t *frame,
                               const ducar_can_id_fields_t *fields)
{
    uint8_t u8Status;

    if ((frame->dlc < 1U) || (boot_can_is_host(fields, 0U) == 0U))
    {
        return;
    }
    switch (frame->data[0])
    {
        case 0x01U:
            (void)boot_can_send_e1_response(fields->sa, DUCAR_CAN_ACK);
            break;
        case 0x02U:
            if (s_boot_can_state == BOOT_CAN_STATE_FINISH_PENDING)
            {
                (void)boot_can_send_e1_response(fields->sa, DUCAR_CAN_NAK);
                break;
            }

            /* A0.1的E1/02不携带固件长度；等待首页TP包头后再校验和擦APP2。 */
            boot_can_reset_state();
            s_boot_can_host_address = fields->sa;
            s_boot_can_state = BOOT_CAN_STATE_WAIT_PAGE;
            s_boot_can_last_activity = boot_timer_get_ms();
            (void)boot_can_send_e1_response(fields->sa, DUCAR_CAN_ACK);
            break;
        case 0x03U:
            u8Status = DUCAR_CAN_NAK;
            if ((s_boot_can_state == BOOT_CAN_STATE_WAIT_PAGE) &&
                (s_boot_can_header_received != 0U) &&
                (s_boot_can_package_received == s_boot_can_package_size) &&
                (s_boot_can_firmware_received == s_boot_can_firmware_size) &&
                boot_can_crc_is_valid() && boot_app_backup_is_valid() &&
                boot_upgrade_prepare_move(s_boot_can_fw_info.size,
                                          s_boot_can_fw_info.crc32))
            {
                u8Status = DUCAR_CAN_ACK;
            }
            if (u8Status == DUCAR_CAN_ACK)
            {
                s_boot_can_tx_failure_baseline = boot_can_get_tx_failed_count();
            }
            if ((u8Status == DUCAR_CAN_ACK) &&
                boot_can_send_e1_response(fields->sa, u8Status))
            {
                s_boot_can_state = BOOT_CAN_STATE_FINISH_PENDING;
                s_boot_can_reset_pending = 1U;
                s_boot_can_last_activity = boot_timer_get_ms();
            }
            else if (u8Status == DUCAR_CAN_ACK)
            {
                boot_can_reset_state();
            }
            else
            {
                (void)boot_can_send_e1_response(fields->sa, u8Status);
            }
            if (u8Status != DUCAR_CAN_ACK)
            {
                boot_can_reset_state();
            }
            break;
        default:
            (void)boot_can_send_e1_response(fields->sa, DUCAR_CAN_NAK);
            break;
    }
}

/* 判断TP连接管理帧是否指向DUCAR升级PGN DF00。 */
static uint8_t boot_can_is_upgrade_pgn(const ducar_can_frame_t *frame)
{
    return (frame->data[5] == 0x00U) &&
           (frame->data[6] == (uint8_t)(BOOT_CAN_UPGRADE_PGN >> 8)) &&
           (frame->data[7] == 0x00U);
}

/* 处理主机RTS，建立一页接收上下文并回复CTS。 */
static void boot_can_handle_rts(const ducar_can_frame_t *frame,
                                const ducar_can_id_fields_t *fields)
{
    uint16_t u16PageMessageLength;

    if ((frame->dlc < 8U) || (boot_can_is_host(fields, 1U) == 0U) ||
        (frame->data[0] != 0x10U) || (!boot_can_is_upgrade_pgn(frame)))
    {
        return;
    }
    u16PageMessageLength = ducar_can_read_u16_le(&frame->data[1]);
    if ((s_boot_can_state != BOOT_CAN_STATE_WAIT_PAGE) ||
        (u16PageMessageLength != BOOT_CAN_PAGE_MESSAGE_LEN) ||
        (frame->data[3] != BOOT_CAN_PAGE_PACKET_COUNT))
    {
        (void)boot_can_send_abort(fields->sa);
        boot_can_reset_state();
        return;
    }
    memset(s_boot_can_page_buffer, 0xFF, sizeof(s_boot_can_page_buffer));
    s_boot_can_page_length = u16PageMessageLength;
    s_boot_can_page_packets = frame->data[3];
    s_boot_can_expected_packet = 1U;
    s_boot_can_state = BOOT_CAN_STATE_RECEIVING_PAGE;
    s_boot_can_last_activity = boot_timer_get_ms();
    if (!boot_can_send_cts(fields->sa))
    {
        boot_can_reset_state();
    }
}

/* 按序接收DT；收齐一页后写Flash、回EOM并等待下一页或E1/03。 */
static void boot_can_handle_dt(const ducar_can_frame_t *frame,
                               const ducar_can_id_fields_t *fields)
{
    uint16_t u16PageBufferOffset;
    uint16_t u16PageBytesRemaining;
    uint8_t u8PageCopyLength;

    if ((boot_can_is_host(fields, 1U) == 0U) ||
        (s_boot_can_state != BOOT_CAN_STATE_RECEIVING_PAGE))
    {
        return;
    }
    if (frame->data[0] != s_boot_can_expected_packet)
    {
        (void)boot_can_send_abort(fields->sa);
        boot_can_reset_state();
        return;
    }
    u16PageBufferOffset = (uint16_t)((uint16_t)(s_boot_can_expected_packet - 1U) * 7U);
    u16PageBytesRemaining = (uint16_t)(s_boot_can_page_length - u16PageBufferOffset);
    u8PageCopyLength = (u16PageBytesRemaining > 7U) ? 7U : (uint8_t)u16PageBytesRemaining;
    if (frame->dlc < (uint8_t)(u8PageCopyLength + 1U))
    {
        (void)boot_can_send_abort(fields->sa);
        boot_can_reset_state();
        return;
    }
    memcpy(&s_boot_can_page_buffer[u16PageBufferOffset], &frame->data[1], u8PageCopyLength);
    s_boot_can_last_activity = boot_timer_get_ms();
    if (s_boot_can_expected_packet >= s_boot_can_page_packets)
    {
        if ((!boot_can_write_page()) || (!boot_can_send_eom(fields->sa)))
        {
            (void)boot_can_send_abort(fields->sa);
            boot_can_reset_state();
            return;
        }
        s_boot_can_state = BOOT_CAN_STATE_WAIT_PAGE;
    }
    else
    {
        s_boot_can_expected_packet++;
    }
}

/* 初始化BOOT升级状态机并保存底层CAN发送回调。 */
void boot_can_upgrade_init(boot_can_upgrade_tx_callback_t tx_callback)
{
    s_boot_can_tx = tx_callback;
    boot_can_reset_state();
}

/* 周期处理接收超时、E1/03后的发送完成等待以及最终系统复位。 */
void boot_can_upgrade_task(void)
{
    uint32_t u32CurrentTimeMs = boot_timer_get_ms();
    uint8_t u8CanTxIdle;

    if (((s_boot_can_state == BOOT_CAN_STATE_RECEIVING_PAGE) ||
         (s_boot_can_state == BOOT_CAN_STATE_WAIT_PAGE)) &&
        ((uint32_t)(u32CurrentTimeMs - s_boot_can_last_activity) >= BOOT_CAN_TIMEOUT_MS))
    {
        (void)boot_can_send_abort(s_boot_can_host_address);
        boot_can_reset_state();
        return;
    }
    if (s_boot_can_reset_pending != 0U)
    {
        u8CanTxIdle = boot_can_is_tx_idle() ? 1U : 0U;
        if (boot_can_get_tx_failed_count() != s_boot_can_tx_failure_baseline)
        {
            boot_can_reset_state();
        }
        else if ((uint32_t)(u32CurrentTimeMs - s_boot_can_last_activity) >=
                 BOOT_CAN_FINISH_TIMEOUT_MS)
        {
            boot_can_reset_state();
        }
        else if (u8CanTxIdle != 0U)
        {
            NVIC_SystemReset();
        }
    }
}

/* BOOT协议接收总入口：先校验ID，再按PF分发版本查询、E1、RTS和DT。 */
bool boot_can_upgrade_on_receive(const ducar_can_frame_t *frame)
{
    ducar_can_id_fields_t tCanIdFields;

    if ((frame == 0) || (frame->is_extended == 0U) ||
        (frame->dlc > DUCAR_CAN_MAX_DATA_LEN) ||
        (!ducar_can_validate_id(frame->id, &tCanIdFields)))
    {
        return false;
    }
    /* BOOT独立运行时也支持HOST查询F200，固定回复FD FE FF。 */
    if ((tCanIdFields.pf == DUCAR_CAN_PF_REQUEST) &&
        boot_can_is_version_request(frame, &tCanIdFields))
    {
        (void)boot_can_send_version_response();
        return true;
    }
    if (tCanIdFields.pf == DUCAR_CAN_PF_BOOT_CONTROL)
    {
        boot_can_handle_e1(frame, &tCanIdFields);
        return (boot_can_is_host(&tCanIdFields, 0U) != 0U);
    }
    if ((tCanIdFields.pf == DUCAR_CAN_PF_TP_CM) && (frame->dlc >= 8U) &&
        (frame->data[0] == 0x10U) && boot_can_is_upgrade_pgn(frame))
    {
        boot_can_handle_rts(frame, &tCanIdFields);
        return true;
    }
    if ((tCanIdFields.pf == DUCAR_CAN_PF_TP_DP) && (frame->dlc >= 2U))
    {
        boot_can_handle_dt(frame, &tCanIdFields);
        return (boot_can_is_host(&tCanIdFields, 1U) != 0U);
    }
    return false;
}
