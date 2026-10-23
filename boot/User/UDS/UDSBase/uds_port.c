#include "uds_port.h"

#include <string.h>

#include "Boot/boot_can_upgrade.h"
#include "Hal_Driver/boot_can.h"
#include "Hal_Driver/boot_timer.h"
#include "n32g45x.h"
#include "uds_service.h"
#include "uds_tp.h"

static uint8_t s_uds_reset_after_tx;

void uds_recv_frame(uint32_t id, uint8_t *frame_buf, uint8_t frame_dlc)
{
    if ((id == REQUEST_ID) || (id == FUNCTIONAL_REQUEST_ID))
    {
        uds_tp_recv_frame((id == FUNCTIONAL_REQUEST_ID) ? 1U : 0U,
                          frame_buf,
                          frame_dlc);
    }
}

int uds_send_frame(uint32_t response_id, uint8_t *frame_buf, uint8_t frame_dlc)
{
    ducar_can_frame_t frame;

    if ((response_id != RESPONSE_ID) || (frame_buf == 0) ||
        (frame_dlc > DUCAR_CAN_MAX_DATA_LEN))
    {
        return -1;
    }

    ducar_can_frame_init(&frame, response_id);
    frame.is_extended = 0U;
    frame.dlc = frame_dlc;
    memcpy(frame.data, frame_buf, frame_dlc);
    return boot_can_send_frame(&frame) ? 0 : -1;
}

void uds_request_reset_after_tx(void)
{
    s_uds_reset_after_tx = 1U;
}

uint8_t uds_reset_after_tx_pending(void)
{
    return s_uds_reset_after_tx;
}

uint32_t uds_port_get_time_ms(void)
{
    return boot_timer_get_ms();
}

void uds_init(void)
{
    s_uds_reset_after_tx = 0U;
    (void)service_init();
}

void uds_1ms_task(void)
{
    network_task();
    service_task();
    if ((s_uds_reset_after_tx != 0U) && boot_can_is_tx_idle())
    {
        s_uds_reset_after_tx = 0U;
        NVIC_SystemReset();
    }
}
