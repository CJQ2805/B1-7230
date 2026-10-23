#include "boot_protocol.h"

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Boot/boot_upgrade.h"
#include "Hal_Driver/boot_flash.h"
#include "Hal_Driver/boot_timer.h"
#include "Hal_Driver/boot_uart.h"
#include "Hal_Driver/boot_watchdog.h"
#include "Protocol/ducar_iap_protocol.h"
#include "SEGGER_RTT.h"
#include "n32g45x.h"
#include <string.h>

typedef enum
{
    BOOT_PROTOCOL_STATE_IDLE = 0U,      /* 未进入升级，等待Update Start。 */
    BOOT_PROTOCOL_STATE_READY,          /* 已收到Update Start，等待第0帧元数据。 */
    BOOT_PROTOCOL_STATE_RECEIVING,      /* 正在按帧号顺序写APP2。 */
    BOOT_PROTOCOL_STATE_FINISHED        /* APP2已经收完整并通过CRC校验。 */
} boot_protocol_state_t;

static ducar_iap_parser_t s_boot_protocol_parser;                  /* 串口协议解析器。 */
static boot_protocol_state_t s_boot_protocol_state;             /* BOOT侧升级接收状态。 */
static ducar_iap_update_info_t s_boot_protocol_update_info;        /* Update Start里的页数量和页大小。 */
static ducar_iap_firmware_info_t s_boot_protocol_fw_info;          /* 第0帧解析出的固件信息。 */
static uint16_t s_boot_protocol_expected_frame;                 /* 下一帧期望收到的Program帧号。 */
static uint16_t s_boot_protocol_total_data_frames;              /* 固件真实数据需要的Program帧数，不含第0帧。 */
static uint8_t s_boot_protocol_backup_ready;                    /* APP2备份固件是否已通过校验。 */
static uint8_t s_boot_protocol_tx_frame[DUCAR_IAP_MAX_FRAME_LEN];  /* 应答帧发送缓冲。 */

static bool boot_protocol_update_info_is_valid(const ducar_iap_update_info_t *info)
{
    uint16_t max_pages = (uint16_t)((BOOT_APP_CODE_SIZE / (uint32_t)DUCAR_IAP_PAGE_SIZE) + 1UL);

    if (info == 0)
    {
        return false;
    }

    return ((info->page_size == DUCAR_IAP_PAGE_SIZE) &&
            (info->page_count >= 2U) &&
            (info->page_count <= max_pages));
}

static bool boot_protocol_page_count_matches(uint16_t update_page_count, uint16_t data_frames)
{
    /*
     * 协议示例里Update Start的页数量包含第0帧，所以应等于data_frames+1。
     * 为了兼容上位机只填写真实数据帧数的情况，这里也接受data_frames。
     */
    return ((update_page_count == (uint16_t)(data_frames + 1U)) ||
            (update_page_count == data_frames));
}

static void boot_protocol_reset_update_state(void)
{
    s_boot_protocol_state = BOOT_PROTOCOL_STATE_IDLE;
    s_boot_protocol_expected_frame = 0U;
    s_boot_protocol_total_data_frames = 0U;
    s_boot_protocol_backup_ready = 0U;
    memset(&s_boot_protocol_update_info, 0, sizeof(s_boot_protocol_update_info));
    memset(&s_boot_protocol_fw_info, 0, sizeof(s_boot_protocol_fw_info));
}

static bool boot_protocol_erase_app2(void)
{
    uint32_t addr;

    for (addr = BOOT_APP2_START_ADDR; addr < BOOT_APP2_END_ADDR; addr += BOOT_FLASH_PAGE_SIZE)
    {
        boot_watchdog_feed();
        if (!boot_flash_erase_page(addr))
        {
            return false;
        }
    }

    return true;
}

static bool boot_protocol_backup_crc_is_valid(void)
{
    uint32_t crc;

    if ((s_boot_protocol_fw_info.size == 0UL) || (s_boot_protocol_fw_info.size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    crc = ducar_iap_crc32_calc((const uint8_t *)BOOT_APP2_START_ADDR, s_boot_protocol_fw_info.size);
    return (crc == s_boot_protocol_fw_info.crc32);
}

static bool boot_protocol_app1_crc_is_valid(void)
{
    uint32_t crc;

    if ((s_boot_protocol_fw_info.size == 0UL) || (s_boot_protocol_fw_info.size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    crc = ducar_iap_crc32_calc((const uint8_t *)BOOT_APP1_START_ADDR, s_boot_protocol_fw_info.size);
    return (crc == s_boot_protocol_fw_info.crc32);
}

static bool boot_protocol_backup_is_valid(void)
{
    return (boot_app_backup_is_valid() && boot_protocol_backup_crc_is_valid());
}

static bool boot_protocol_move_backup_to_app1(void)
{
    if ((!boot_upgrade_prepare_move(s_boot_protocol_fw_info.size,
                                    s_boot_protocol_fw_info.crc32)) ||
        (!boot_upgrade_move_backup_to_app1()))
    {
        return false;
    }

    return (boot_protocol_app1_crc_is_valid() && boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_APP_VALID));
}

static void boot_protocol_send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint16_t frame_len;

    frame_len = ducar_iap_pack_frame(cmd, data, len, s_boot_protocol_tx_frame, (uint16_t)sizeof(s_boot_protocol_tx_frame));
    if (frame_len > 0U)
    {
        boot_uart_send_buffer(s_boot_protocol_tx_frame, frame_len);
    }
}

static void boot_protocol_send_status(uint8_t cmd, uint8_t status)
{
    boot_protocol_send_frame(cmd, &status, 1U);
}

static void boot_protocol_finish_receive(void)
{
    s_boot_protocol_backup_ready = 1U;
    s_boot_protocol_state = BOOT_PROTOCOL_STATE_FINISHED;

    /*
     * 最后一包已经写入APP2并完成CRC校验，先回复PROGRAM ACK，避免上位机误判最后一包失败。
     * ACK发送完成后进入FINISHED状态，等待上位机JUMP命令触发APP2到APP1的搬运。
     */
    boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_ACK);
    boot_uart_wait_tx_complete();
}

static void boot_protocol_move_and_jump_app1(void)
{
    (void)SEGGER_RTT_WriteString(0U, "BOOT move APP2 to APP1\r\n");
    if (!boot_protocol_move_backup_to_app1())
    {
        (void)boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_WAIT_PACKAGE);
        boot_protocol_reset_update_state();
        return;
    }

    (void)SEGGER_RTT_WriteString(0U, "BOOT jump APP1\r\n");
    (void)boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_APP_VALID);
    NVIC_SystemReset();
    while (1)
    {
    }
}

static void boot_protocol_send_check_in(void)
{
    uint8_t data[2];

    data[0] = DUCAR_IAP_ACK;
    data[1] = DUCAR_IAP_IN_BOOT;
    boot_protocol_send_frame(DUCAR_IAP_CMD_CHECK_IN, data, (uint8_t)sizeof(data));
}

static void boot_protocol_handle_check(void)
{
    /*
     * CHECK用于确认最终是否已经运行到APP。
     * 如果BOOT还能收到并处理CHECK，说明还没有成功进入APP，因此明确返回NAK。
     */
    boot_protocol_send_status(DUCAR_IAP_CMD_CHECK, DUCAR_IAP_NAK);
}

static void boot_protocol_handle_update_start(const ducar_iap_frame_t *frame)
{
    ducar_iap_update_info_t update_info;

    if ((!ducar_iap_parse_update_start(frame, &update_info)) || (!boot_protocol_update_info_is_valid(&update_info)))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    boot_protocol_reset_update_state();
    if (!boot_protocol_erase_app2())
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_NAK);
        return;
    }

    s_boot_protocol_update_info = update_info;
    s_boot_protocol_state = BOOT_PROTOCOL_STATE_READY;
    (void)boot_upgrade_set_flag(BOOT_UPGRADE_FLAG_WAIT_PACKAGE);
    boot_protocol_send_status(DUCAR_IAP_CMD_UPDATE_START, DUCAR_IAP_ACK);
}

static void boot_protocol_handle_program_info(const ducar_iap_frame_t *frame)
{
    ducar_iap_firmware_info_t fw_info;
    uint16_t data_frames;

    if ((s_boot_protocol_state != BOOT_PROTOCOL_STATE_READY) || (!ducar_iap_parse_firmware_info(frame, &fw_info)) ||
        (!ducar_iap_part_number_is_allowed(BOOT_PARAM_SOFTWARE_PN, fw_info.part_number)) ||
        (fw_info.size == 0UL) || (fw_info.size > BOOT_APP_CODE_SIZE))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    data_frames = ducar_iap_calc_data_frames(fw_info.size);
    if ((data_frames == 0U) || (!boot_protocol_page_count_matches(s_boot_protocol_update_info.page_count, data_frames)))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    s_boot_protocol_fw_info = fw_info;
    s_boot_protocol_total_data_frames = data_frames;
    s_boot_protocol_expected_frame = 1U;
    s_boot_protocol_state = BOOT_PROTOCOL_STATE_RECEIVING;
    boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_ACK);
}

static void boot_protocol_handle_program_data(const ducar_iap_frame_t *frame, uint16_t frame_no)
{
    uint32_t offset;
    uint32_t remain;
    uint32_t write_len;

    if ((s_boot_protocol_state != BOOT_PROTOCOL_STATE_RECEIVING) || (frame_no != s_boot_protocol_expected_frame) ||
        (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    offset = ((uint32_t)frame_no - 1UL) * (uint32_t)DUCAR_IAP_PAGE_SIZE;
    if (offset >= s_boot_protocol_fw_info.size)
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    remain = s_boot_protocol_fw_info.size - offset;
    write_len = (remain > (uint32_t)DUCAR_IAP_PAGE_SIZE) ? (uint32_t)DUCAR_IAP_PAGE_SIZE : remain;
    if (!boot_flash_write(BOOT_APP2_START_ADDR + offset, &frame->data[2], write_len))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    if (frame_no >= s_boot_protocol_total_data_frames)
    {
        if (!boot_protocol_backup_is_valid())
        {
            boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
            boot_protocol_reset_update_state();
            return;
        }

        boot_protocol_finish_receive();
        return;
    }
    else
    {
        s_boot_protocol_expected_frame++;
    }

    boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_ACK);
}

static void boot_protocol_handle_program(const ducar_iap_frame_t *frame)
{
    uint16_t frame_no;

    if ((frame == 0) || (frame->len != DUCAR_IAP_PROGRAM_DATA_LEN))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_PROGRAM, DUCAR_IAP_NAK);
        boot_protocol_reset_update_state();
        return;
    }

    frame_no = ducar_iap_get_frame_no(frame);
    if (frame_no == 0U)
    {
        boot_protocol_handle_program_info(frame);
    }
    else
    {
        boot_protocol_handle_program_data(frame, frame_no);
    }
}

static void boot_protocol_handle_jump(void)
{
    /*
     * JUMP命令用于触发BOOT搬运和跳转。
     * 必须等APP2完整接收并通过CRC校验后，才能执行APP2到APP1的搬运。
     */
    if ((s_boot_protocol_state != BOOT_PROTOCOL_STATE_FINISHED) || (s_boot_protocol_backup_ready == 0U))
    {
        boot_protocol_send_status(DUCAR_IAP_CMD_JUMP, DUCAR_IAP_NAK);
        return;
    }

    boot_protocol_send_status(DUCAR_IAP_CMD_JUMP, DUCAR_IAP_ACK);
    boot_uart_wait_tx_complete();
    boot_timer_delay_ms(2UL);
    boot_protocol_move_and_jump_app1();
}

static void boot_protocol_handle_frame(const ducar_iap_frame_t *frame)
{
    switch (frame->cmd)
    {
        case DUCAR_IAP_CMD_CHECK_IN:
            boot_protocol_send_check_in();
            break;

        case DUCAR_IAP_CMD_UPDATE_START:
            boot_protocol_handle_update_start(frame);
            break;

        case DUCAR_IAP_CMD_PROGRAM:
            boot_protocol_handle_program(frame);
            break;

        case DUCAR_IAP_CMD_JUMP:
            boot_protocol_handle_jump();
            break;

        case DUCAR_IAP_CMD_CHECK:
            boot_protocol_handle_check();
            break;

        default:
            break;
    }
}

void boot_protocol_init(void)
{
    ducar_iap_parser_init(&s_boot_protocol_parser);
    boot_protocol_reset_update_state();
}

void boot_protocol_task(void)
{
    uint8_t byte;
    ducar_iap_frame_t frame;
    ducar_iap_parse_result_t parse_result;

    while (boot_uart_read_byte(&byte))
    {
        boot_watchdog_feed();
        parse_result = ducar_iap_parser_input(&s_boot_protocol_parser, byte, &frame);
        if (parse_result == DUCAR_IAP_PARSE_OK)
        {
            boot_protocol_handle_frame(&frame);
        }
        else if (parse_result == DUCAR_IAP_PARSE_ERROR)
        {
            if ((frame.cmd == DUCAR_IAP_CMD_UPDATE_START) ||
                (frame.cmd == DUCAR_IAP_CMD_PROGRAM) ||
                (frame.cmd == DUCAR_IAP_CMD_JUMP) ||
                (frame.cmd == DUCAR_IAP_CMD_CHECK))
            {
                boot_protocol_send_status(frame.cmd, DUCAR_IAP_NAK);
                boot_protocol_reset_update_state();
            }
        }
        else
        {
            /* 未组成完整帧时继续轮询串口。 */
        }
    }
}

