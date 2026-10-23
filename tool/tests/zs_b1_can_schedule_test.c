#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "App/app_can.h"
#include "Driver/d_can.h"
#include "Protocol/Comm/zs_b1_can_protocol.h"
#include "Protocol/Comm/ducar_can_protocol.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%u: %s\n", __func__, (unsigned)__LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct d_can_t { unsigned unused; };
d_can_t can0;
static unsigned zs_init_count;
static unsigned zs_task_count;
static bool client_protocol_sleep_allowed = true;

void zs_b1_can_protocol_init(void) { zs_init_count++; }
void zs_b1_can_protocol_task(void) { zs_task_count++; }
bool zs_b1_can_protocol_allows_sleep(void) { return client_protocol_sleep_allowed; }

bool d_can_init(d_can_t *can) { (void)can; return true; }
void d_can_deinit(d_can_t *can) { (void)can; }
void d_can_set_extended_tx_enabled(bool enabled) { (void)enabled; }
bool d_can_send(d_can_t *can, uint32_t id, d_can_id_t type, const uint8_t *data, uint8_t len)
{ (void)can; (void)id; (void)type; (void)data; (void)len; return true; }
bool d_can_is_tx_idle(d_can_t *can) { (void)can; return true; }
bool d_can_read(t_can_packet *packet) { (void)packet; return false; }

void ducar_can_protocol_init(void) {}
void ducar_can_data_renew(void) {}
void ducar_can_bus_timer_handle(void) {}
bool ducar_can_detd_tx_window_is_active(void) { return false; }
void ducar_can_50ms_task(void) {}
void ducar_can_100ms_task(void) {}
void ducar_can_200ms_task(void) {}
void ducar_can_500ms_task(void) {}
void ducar_can_1000ms_task(void) {}
void ducar_can_protocol_receive(const t_can_packet *packet) { (void)packet; }
void ducar_can_control_reply_task(void) {}
bool ducar_can_id_is_pc_source(const ducar_can_id_fields_t *fields) { (void)fields; return false; }

void ducar_can_iap_protocol_init(void) {}
bool ducar_can_iap_protocol_is_packet(const t_can_packet *packet) { (void)packet; return false; }
void ducar_can_iap_protocol_receive(const t_can_packet *packet) { (void)packet; }
void ducar_can_iap_protocol_task(void) {}
bool ducar_iap_is_busy(void) { return false; }

void uds_1ms_task(void) {}
void uds_init(void) {}
bool uds_normal_receive_enabled(void) { return true; }
void uds_recv_frame(uint32_t id, const uint8_t *data, uint8_t len)
{ (void)id; (void)data; (void)len; }
void service_28_reset(void) {}
void SEGGER_RTT_WriteString(unsigned buffer, const char *text)
{ (void)buffer; (void)text; }

int main(void)
{
    CHECK(app_can_init());
    CHECK(zs_init_count == 1U);

    CAN_TransmitTask();
    CHECK(zs_task_count == 0U);
    CAN_TransmitTask();
    CHECK(zs_task_count == 0U);

    client_protocol_sleep_allowed = false;
    CHECK(!app_can_is_sleep_allowed());
    client_protocol_sleep_allowed = true;
    CHECK(app_can_is_sleep_allowed());

    puts("PASS zs_b1_can_schedule_test");
    return 0;
}
