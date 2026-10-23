/******************************************************************************
 * Customer CommunicationControl: 28 83 03 mutes normal DUCAR traffic;
 * 28 80 03 restores it. Standard-ID UDS diagnostic traffic stays available.
 ******************************************************************************/
#include "SID28_CommunicationControl.h"

#include "Driver/d_can.h"
#include "Driver/d_rtc.h"
#include "SID10_SessionControl.h"
#include "SID27_SecurityAccess.h"
#include "service_cfg.h"
#include "uds_service.h"

#define UDS_COMM_CONTROL_MAX_MUTE_SECONDS  301UL
#define UDS_COMM_CONTROL_POLL_PERIOD_MS    1000U

static bool_t s_normal_transmit_enabled = TRUE;
static bool_t s_normal_receive_enabled = TRUE;
static bool_t s_mute_timeout_active;
static uint32_t s_mute_elapsed_seconds;
static uint32_t s_mute_last_rtc_seconds;
static uint16_t s_mute_poll_ms;

static bool_t service_28_start_mute_timeout(void)
{
    uint32_t rtc_seconds;

    if (s_mute_timeout_active == TRUE)
    {
        /* Repeated 28 83 cannot extend the original five-minute ceiling. */
        return TRUE;
    }
    if (!d_rtc_read_seconds_since_2000(&rtc_seconds))
    {
        return FALSE;
    }

    s_mute_last_rtc_seconds = rtc_seconds;
    s_mute_elapsed_seconds = 0UL;
    s_mute_poll_ms = 0U;
    s_mute_timeout_active = TRUE;
    return TRUE;
}

bool_t service_28_check_len(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    return ((msg_buf != 0) && (msg_dlc == 3U)) ? TRUE : FALSE;
}

bool_t uds_normal_receive_enabled(void)
{
    return s_normal_receive_enabled;
}

bool_t uds_normal_transmit_enabled(void)
{
    return s_normal_transmit_enabled;
}

void service_28_reset(void)
{
    s_normal_transmit_enabled = TRUE;
    s_normal_receive_enabled = TRUE;
    s_mute_timeout_active = FALSE;
    s_mute_elapsed_seconds = 0UL;
    s_mute_last_rtc_seconds = 0UL;
    s_mute_poll_ms = 0U;
    d_can_set_extended_tx_enabled(true);
}

/* RTC calendar seconds include STOP sleep; a backward clock correction rebases without expiring early. */
bool_t service_28_timeout_poll(void)
{
    uint32_t rtc_seconds;
    uint32_t elapsed_seconds;

    if (s_mute_timeout_active != TRUE)
    {
        return FALSE;
    }
    if (!d_rtc_read_seconds_since_2000(&rtc_seconds))
    {
        /* Fail open if the RTC stops or becomes unreadable; never mute indefinitely. */
        service_28_reset();
        return TRUE;
    }

    if (rtc_seconds >= s_mute_last_rtc_seconds)
    {
        elapsed_seconds = rtc_seconds - s_mute_last_rtc_seconds;
    }
    else
    {
        /* RTC was corrected backwards; restart the delta baseline instead of counting a false day rollover. */
        elapsed_seconds = 0UL;
    }
    s_mute_last_rtc_seconds = rtc_seconds;
    if (elapsed_seconds > 0UL)
    {
        if (elapsed_seconds >= (UDS_COMM_CONTROL_MAX_MUTE_SECONDS - s_mute_elapsed_seconds))
        {
            service_28_reset();
            return TRUE;
        }
        s_mute_elapsed_seconds += elapsed_seconds;
    }
    return FALSE;
}

/* UDS task runs while awake; the low-power manager separately polls after every RTC wake. */
void service_28_timeout_task_1ms(void)
{
    if (s_mute_timeout_active != TRUE)
    {
        return;
    }
    s_mute_poll_ms++;
    if (s_mute_poll_ms >= UDS_COMM_CONTROL_POLL_PERIOD_MS)
    {
        s_mute_poll_ms = 0U;
        (void)service_28_timeout_poll();
    }
}

/* S3 expiry must not shorten the explicit five-minute minimum mute window. */
void service_28_on_s3_timeout(void)
{
    if (s_mute_timeout_active != TRUE)
    {
        service_28_reset();
    }
}

void service_28_CommunicationControl(const uint8_t *msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t rsp_buf[2];

    if ((msg_buf == 0) || (msg_dlc != 3U) || (msg_buf[2] != 0x03U))
    {
        uds_negative_rsp(SID_28, NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    subfunction = (uint8_t)(msg_buf[1] & 0x7FU);
    if (subfunction == 0x03U)
    {
        if (get_current_session() != UDS_SESSION_EXT)
        {
            uds_negative_rsp(SID_28, NRC_SUBFUNCTION_NOT_SUPPORTED_IN_ACTIVE_SESSION);
            return;
        }
        if (get_current_sa_lv() < UDS_SA_LV1)
        {
            uds_negative_rsp(SID_28, NRC_SECURITY_ACCESS_DENIED);
            return;
        }
        if (!service_28_start_mute_timeout())
        {
            uds_negative_rsp(SID_28, NRC_CONDITIONS_NOT_CORRECT);
            return;
        }
        s_normal_transmit_enabled = FALSE;
        s_normal_receive_enabled = FALSE;
        d_can_set_extended_tx_enabled(false);
    }
    else if (subfunction == 0x00U)
    {
        /* A manual resume ends this mute window; the next 28 83 starts a fresh RTC deadline. */
        service_28_reset();
    }
    else
    {
        uds_negative_rsp(SID_28, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_28);
    rsp_buf[1] = msg_buf[1];
    uds_positive_rsp(rsp_buf, sizeof(rsp_buf));
}
