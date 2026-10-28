#include <stdio.h>
#include <string.h>

#include "Driver/d_can.h"
#include "App/app_param.h"
#include "App/app_bms_status.h"
#include "App/app_bms_snapshot.h"
#include "../../app/N32G45x_KEIL/User_Src/Protocol/Comm/zs_b1_can_protocol.c"

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL %s:%u: %s\n", __func__, (unsigned)__LINE__, #condition); \
        failures++; \
        return; \
    } \
} while (0)

struct d_can_t { unsigned unused; };
d_can_t can0;
static app_bms_snapshot_t sample;
#define TEST_FRAME_CAPACITY 128U
static t_can_packet frames[TEST_FRAME_CAPACITY];
static t_can_packet first_frame;
static t_can_packet last_frame;
static unsigned sent_count;
static unsigned failures;

bool d_can_send(d_can_t *can, uint32_t id, d_can_id_t type, const uint8_t *data, uint8_t len)
{
    t_can_packet packet = {0};
    (void)can;
    packet.u32id = id;
    packet.u32idType = (uint32_t)type;
    packet.u8len = len;
    if (len <= D_CAN_MAX_DATA_LEN) memcpy(packet.au8data, data, len);
    if (sent_count == 0U) first_frame = packet;
    last_frame = packet;
    if (sent_count < TEST_FRAME_CAPACITY) frames[sent_count] = packet;
    sent_count++;
    return true;
}

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot)
{
    *snapshot = sample;
}

static void reset_case(void)
{
    memset(&sample, 0, sizeof(sample));
    memset(frames, 0, sizeof(frames));
    memset(&first_frame, 0, sizeof(first_frame));
    memset(&last_frame, 0, sizeof(last_frame));
    sent_count = 0U;
    zs_b1_can_protocol_init();
}

static const t_can_packet *find_frame(uint32_t id)
{
    unsigned index;
    unsigned available = (sent_count < TEST_FRAME_CAPACITY) ? sent_count : TEST_FRAME_CAPACITY;
    for (index = 0U; index < available; index++)
    {
        if (frames[index].u32id == id) return &frames[index];
    }
    return 0;
}

static unsigned count_frames(uint32_t id)
{
    unsigned index;
    unsigned count = 0U;
    unsigned available = (sent_count < TEST_FRAME_CAPACITY) ? sent_count : TEST_FRAME_CAPACITY;
    for (index = 0U; index < available; index++)
    {
        if (frames[index].u32id == id) count++;
    }
    return count;
}

static const t_can_packet *transmit_fault_frame(void)
{
    unsigned tick;

    sample.signal.input.mcu_detd = 1U;
    zs_b1_can_protocol_task();
    for (tick = 0U; tick < 100U; tick++) zs_b1_can_protocol_task();
    return find_frame(0x019U);
}

static const t_can_packet *send_fault_frame(uint8_t bms_status, uint8_t detc_present)
{
    reset_case();
    sample.bms_work_status = bms_status;
    sample.signal.input.mcu_detc = detc_present;
    return transmit_fault_frame();
}

static void test_work_mode_uses_bms_status_and_maps_feedback_to_discharge(void)
{
    const t_can_packet *packet;

    packet = send_fault_frame(APP_BMS_STATUS_STATIC, 0U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x30U) == 0x00U);

    packet = send_fault_frame(APP_BMS_STATUS_DISCHARGE, 0U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x30U) == 0x10U);

    packet = send_fault_frame(APP_BMS_STATUS_FEEDBACK, 0U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x30U) == 0x10U);

    packet = send_fault_frame(APP_BMS_STATUS_CHARGE, 0U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x30U) == 0x20U);
}

static void test_work_mode_fault_overrides_state_and_detc_bit6_is_presence(void)
{
    const t_can_packet *packet;

    packet = send_fault_frame(APP_BMS_STATUS_CHARGE, 1U);
    CHECK(packet != 0);
    CHECK((packet->au8data[1] & 0x30U) == 0x20U);
    CHECK((packet->au8data[1] & 0x40U) == 0x40U);

    reset_case();
    sample.bms_work_status = APP_BMS_STATUS_STATIC;
    sample.signal.input.mcu_detd = 1U;
    sample.fault.active.charge_pro = 1U;
    zs_b1_can_protocol_task();
    for (unsigned tick = 0U; tick < 100U; tick++) zs_b1_can_protocol_task();
    packet = find_frame(0x019U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x30U) == 0x30U);

    reset_case();
    sample.bms_work_status = APP_BMS_STATUS_STATIC;
    sample.signal.input.mcu_detd = 1U;
    sample.signal.input.mcu_detc = 0U;
    zs_b1_can_protocol_task();
    for (unsigned tick = 0U; tick < 100U; tick++) zs_b1_can_protocol_task();
    packet = find_frame(0x019U);
    CHECK(packet != 0 && (packet->au8data[1] & 0x40U) == 0x00U);
}

static void test_fault_level_maps_discharge_warning_and_protection(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x03U) == 0x02U);

    reset_case();
    sample.fault.active.discharge_pro = 1U;
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x03U) == 0x03U);

    /* A charge-only protection must not be reported as a discharge fault. */
    reset_case();
    sample.fault.active.charge_pro = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x03U) == 0x00U);
}

static void test_charge_status_uses_bms_status_and_full_charge_has_priority(void)
{
    const t_can_packet *packet;

    packet = send_fault_frame(APP_BMS_STATUS_CHARGE, 1U);
    CHECK(packet != 0 && (packet->au8data[2] & 0x0CU) == 0x08U);

    reset_case();
    sample.bms_work_status = APP_BMS_STATUS_CHARGE;
    sample.signal.input.mcu_detc = 1U;
    sample.fault.active.charge_pro = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x0CU) == 0x04U);

    reset_case();
    sample.bms_work_status = APP_BMS_STATUS_CHARGE;
    sample.signal.input.mcu_detc = 1U;
    sample.fault.active.charge_pro = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x0CU) == 0x0CU);

    packet = send_fault_frame(APP_BMS_STATUS_STATIC, 0U);
    CHECK(packet != 0 && (packet->au8data[2] & 0x0CU) == 0x00U);
}

static void test_status_feedback_reports_only_valid_dsg_mos_state(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.afe_status_valid = 1U;
    sample.mos.dsg_mos_state = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x30U) == 0x10U);

    reset_case();
    sample.afe_status_valid = 1U;
    sample.mos.chg_mos_state = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x30U) == 0x20U);

    reset_case();
    sample.mos.dsg_mos_state = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[2] & 0x30U) == 0x00U);
}

static void test_fault_frame_reports_pack_and_cell_voltage_sources(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_war_pov = 1U;
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov = 1U;
    sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv = 1U;
    sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && packet->au8data[3] == 0x3FU);

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2 = 1U;
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && packet->au8data[3] == 0xF0U);
    CHECK((packet->au8data[4] & 0x03U) == 0x03U);
}

static void test_cell_uv_fields_report_all_three_levels(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv = 1U;
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0);
    CHECK((packet->au8data[3] & 0xC0U) == 0x40U);
    CHECK((packet->au8data[4] & 0x03U) == 0x01U);

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0);
    CHECK((packet->au8data[3] & 0xC0U) == 0x80U);
    CHECK((packet->au8data[4] & 0x03U) == 0x02U);

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0);
    CHECK((packet->au8data[3] & 0xC0U) == 0xC0U);
    CHECK((packet->au8data[4] & 0x03U) == 0x03U);
}

static void test_overtemperature_fields_report_warning_and_protection_levels(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_war_otc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[4] & 0x3CU) == 0x14U);

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[4] & 0x0CU) == 0x08U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[4] & 0x30U) == 0x20U);
}

static void test_under_temperature_fault_fields_report_warning_and_protection_levels(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_war_utd = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_war_utc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0);
    CHECK((packet->au8data[4] & 0xC0U) == 0x40U);
    CHECK((packet->au8data[5] & 0x03U) == 0x01U);

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0);
    CHECK((packet->au8data[4] & 0xC0U) == 0x80U);
    CHECK((packet->au8data[5] & 0x03U) == 0x02U);
}

static void test_charge_overcurrent_fault_field_uses_two_levels_and_maps_lock_to_level_one(void)
{
    const t_can_packet *packet;

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_war_occ = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x00U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x04U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x04U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1reserved_roc_pro = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x00U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 = 1U;
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x08U);

    reset_case();
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x0CU) == 0x04U);
}

static void test_discharge_overcurrent_fault_field_maps_protection_levels_and_saturates_at_three(void)
{
    const t_can_packet *packet;
    t_discharge_fault_B *fault_b;

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x00U);

    reset_case();
    fault_b = &sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B;
    fault_b->b1pack_pro_ocd1 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x10U);

    reset_case();
    fault_b = &sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B;
    fault_b->b1pack_pro_ocd2 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x20U);

    reset_case();
    fault_b = &sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B;
    fault_b->b1pack_pro_ocd3 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x30U);

    reset_case();
    fault_b = &sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B;
    fault_b->b1pack_pro_ocd4 = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x30U);

    reset_case();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0x30U) == 0x30U);
}

static void test_voltage_difference_field_distinguishes_warning_and_protection(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1cell_voltage_diff_waring = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0xC0U) == 0x40U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1cell_voltage_diff_fault = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0xC0U) == 0x80U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1cell_voltage_diff_waring = 1U;
    general_a->b1cell_voltage_diff_fault = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[5] & 0xC0U) == 0x80U);
}

static void test_fault_fields_24_to_26_remain_zero(void)
{
    const t_can_packet *packet;

    reset_case();
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x1FU) == 0U);
}

static void test_fault_field_27_reports_mos_overtemperature_protection_only(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1MOS_OTP_waring = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x20U) == 0U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1MOS_OTP = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x20U) != 0U);
}

static void test_fault_field_28_reports_pf_aggregate_only(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;

    reset_case();
    sample.fault.active.pf_fault = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x40U) != 0U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1MOS_OTP_waring = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x40U) == 0U);
}

static void test_fault_field_29_reports_final_short_circuit_protection_only(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;
    t_discharge_fault_B *discharge_b;

    reset_case();
    discharge_b = &sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B;
    discharge_b->b1pack_shrot = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x80U) != 0U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1SCP_FUSE_fusing = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[6] & 0x80U) == 0U);
}

static void test_fault_field_31_reports_mos_failure_pf_not_other_hardware_fault(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1CHG_MOS_failure = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[7] & 0x02U) != 0U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1DSG_MOS_faulure = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[7] & 0x02U) != 0U);

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1hardware_second_pro = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[7] & 0x02U) == 0U);
}

static void test_fault_fields_30_32_34_remain_zero(void)
{
    const t_can_packet *packet;
    t_general_fault_A *general_a;

    reset_case();
    general_a = &sample.fault.active.detail.ugeneral_fault_A.tgeneral_fault_A;
    general_a->b1AFE_fault = 1U;
    general_a->b1NTC_fault = 1U;
    sample.afe_status_valid = 0U;
    sample.afe_fault.cell_ov = 1U;
    sample.afe_fault.cell_uv = 1U;
    packet = transmit_fault_frame();
    CHECK(packet != 0 && (packet->au8data[7] & 0x1DU) == 0U);
}

static void test_detc_presence_starts_standard_can_transmission(void)
{
    reset_case();
    sample.signal.input.mcu_detc = 1U;
    zs_b1_can_protocol_task();

    CHECK(sent_count == 1U);
    CHECK(first_frame.u32id == 0x419U);
    CHECK(first_frame.u32idType == D_CAN_ID_STANDARD);
    CHECK(first_frame.u8len == 8U);
    CHECK(first_frame.au8data[0] == 2U); /* Network wakeup. */
    CHECK(!zs_b1_can_protocol_allows_sleep());
}

static void test_current_threshold_is_inclusive_in_either_direction(void)
{
    reset_case();
    sample.current_ma = 199;
    zs_b1_can_protocol_task();
    CHECK(sent_count == 0U);

    sample.current_ma = 200;
    zs_b1_can_protocol_task();
    CHECK(sent_count == 1U);
    CHECK(first_frame.u32id == 0x419U);

    reset_case();
    sample.current_ma = -200;
    zs_b1_can_protocol_task();
    CHECK(sent_count == 1U);
    CHECK(first_frame.au8data[0] == 2U);
}

static void test_three_minutes_below_threshold_sends_sleep_then_stops(void)
{
    uint32_t tick;
    reset_case();
    sample.signal.input.mcu_detd = 1U;
    zs_b1_can_protocol_task();

    for (tick = 0U; tick < APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS - 1U; tick++)
    {
        zs_b1_can_protocol_task();
    }
    CHECK(!zs_b1_can_protocol_allows_sleep());

    zs_b1_can_protocol_task();
    CHECK(zs_b1_can_protocol_allows_sleep());
    CHECK(last_frame.u32id == 0x419U);
    CHECK(last_frame.au8data[0] == 1U); /* Network sleep before stopping. */

    tick = sent_count;
    for (uint32_t quiet = 0U; quiet < 1000U; quiet++) zs_b1_can_protocol_task();
    CHECK(sent_count == tick); /* A still-asserted DETD level must not reopen the window. */
}

static void test_current_resets_idle_timer_and_restarts_after_stop(void)
{
    uint32_t tick;
    reset_case();
    sample.signal.input.mcu_detc = 1U;
    zs_b1_can_protocol_task();
    for (tick = 0U; tick < APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS - 1000U; tick++)
    {
        zs_b1_can_protocol_task();
    }

    sample.current_ma = 200;
    zs_b1_can_protocol_task();
    sample.current_ma = 0;
    for (tick = 0U; tick < APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS - 1U; tick++)
    {
        zs_b1_can_protocol_task();
    }
    CHECK(!zs_b1_can_protocol_allows_sleep());

    zs_b1_can_protocol_task();
    CHECK(zs_b1_can_protocol_allows_sleep());

    sample.current_ma = -200;
    zs_b1_can_protocol_task();
    CHECK(!zs_b1_can_protocol_allows_sleep());
    CHECK(last_frame.u32id == 0x419U);
    CHECK(last_frame.au8data[0] == 2U);
}

static void test_protocol_periods_and_voltage_temperature_cell_data(void)
{
    const t_can_packet *packet;
    unsigned tick;

    reset_case();
    sample.bms_work_status = APP_BMS_STATUS_DISCHARGE;
    sample.signal.input.mcu_detd = 1U;
    sample.data_ready = 1U;
    sample.valid_cells = 20U;
    sample.cell_count = 20U;
    sample.cell_mv[0] = 3000U;
    sample.cell_mv[1] = 3200U;
    sample.min_cell_mv = 3000U;
    sample.min_cell_num = 0U;
    sample.max_cell_mv = 4200U;
    sample.max_cell_num = 19U;
    sample.bat_mv = 72000U;
    sample.current_ma = -1250;
    sample.discharging = 1U;
    sample.mos.dsg_mos_state = 1U;
    sample.afe_status_valid = 1U;
    sample.sop.s16MaxDischargeCurrent10mA = 100; /* 10.0A source -> 40 x 0.25A. */
    sample.sop.s16MaxChargeCurrent10mA = 120; /* 12.0A source -> 240 x 0.05A. */
    sample.sop.u16MaxRechargeCurrent10mA = 50; /* 5.0A source -> 20 x 0.25A. */
    sample.sop.u32MaxChargeVoltageMv = 84000U;
    sample.temp_count = 2U;
    sample.temp_valid_mask = 0x03U;
    sample.temp_c[0] = 25;
    sample.temp_c[1] = 35;
    sample.temp_0p1c[0] = 250;
    sample.temp_0p1c[1] = 350;
    sample.min_temp_c = 25;
    sample.max_temp_c = 35;
    sample.min_temp_num = 0U;
    sample.max_temp_num = 1U;
    sample.soc.u32cycle_index = 0x1234U;
    sample.soc.s32battery_full_cap = 30000;
    sample.soc.u8display_soc = 50U;
    sample.soc.u8display_soh = 90U;

    zs_b1_can_protocol_task();
    for (tick = 0U; tick < 500U; tick++) zs_b1_can_protocol_task();

    CHECK(count_frames(0x089U) == 25U);
    CHECK(count_frames(0x019U) == 5U);
    CHECK(count_frames(0x621U) == 5U);
    CHECK(count_frames(0x419U) == 2U); /* Wakeup now, active after 500ms. */
    CHECK(count_frames(0x619U) == 1U);
    CHECK(count_frames(0x519U) == 1U);
    CHECK(count_frames(0x5A0U) == 1U);
    CHECK(count_frames(0x5ABU) == 1U);

    packet = find_frame(0x519U);
    CHECK(packet != 0);
    CHECK(packet->u32idType == D_CAN_ID_STANDARD && packet->u8len == 8U);
    CHECK(packet->au8data[0] == 0x20U && packet->au8data[1] == 0x1CU); /* 72.00V. */
    CHECK(packet->au8data[2] == 0xB8U && packet->au8data[3] == 0x0BU); /* 3000mV. */
    CHECK(packet->au8data[4] == 1U); /* Cell numbers are transmitted 1-based. */
    CHECK(packet->au8data[5] == 0x68U && packet->au8data[6] == 0x10U); /* 4200mV. */
    CHECK(packet->au8data[7] == 20U);

    packet = find_frame(0x5ABU);
    CHECK(packet != 0 && packet->au8data[0] == 1U);
    CHECK(packet->au8data[1] == 0xB8U && packet->au8data[2] == 0x0BU);

    packet = find_frame(0x619U);
    CHECK(packet != 0);
    CHECK(packet->au8data[0] == 0x34U && packet->au8data[1] == 0x12U);
    CHECK(packet->au8data[3] == 0x60U && packet->au8data[4] == 30U);
    CHECK(packet->au8data[5] == 30U && packet->au8data[6] == 100U);
    CHECK(packet->au8data[7] == 90U);

    packet = find_frame(0x089U);
    CHECK(packet != 0 && packet->au8data[0] == 0x40U); /* DETD path can output. */
    CHECK(packet->au8data[1] == 5U && packet->au8data[2] == 0x80U && packet->au8data[3] == 2U);
    CHECK(packet->au8data[4] == 20U && packet->au8data[5] == 0U);

    packet = find_frame(0x019U);
    CHECK(packet != 0 && packet->au8data[0] == 0U && packet->au8data[1] == 0x10U);
    CHECK(packet->au8data[2] == 0x10U);

    packet = find_frame(0x621U);
    CHECK(packet != 0);
    CHECK(packet->au8data[0] == 0xF0U && packet->au8data[1] == 0U && packet->au8data[2] == 0U);
    CHECK(packet->au8data[3] == 0U && packet->au8data[4] == 0U);
    CHECK(packet->au8data[5] == 0xD0U && packet->au8data[6] == 0x20U && packet->au8data[7] == 0x04U);

    packet = find_frame(0x5A0U);
    CHECK(packet != 0 && packet->au8data[0] == 70U); /* average 30C -> offset +40. */
    CHECK(packet->au8data[2] == 65U && packet->au8data[3] == 75U);
    CHECK(packet->au8data[1] == 0x21U);

    packet = find_frame(0x5ABU);
    CHECK(packet != 0 && packet->au8data[4] == 0U && packet->au8data[5] == 1U);
    CHECK(packet->au8data[6] == 0U && packet->au8data[7] == 1U);
}

int main(void)
{
    test_work_mode_uses_bms_status_and_maps_feedback_to_discharge();
    test_work_mode_fault_overrides_state_and_detc_bit6_is_presence();
    test_fault_level_maps_discharge_warning_and_protection();
    test_charge_status_uses_bms_status_and_full_charge_has_priority();
    test_status_feedback_reports_only_valid_dsg_mos_state();
    test_fault_frame_reports_pack_and_cell_voltage_sources();
    test_cell_uv_fields_report_all_three_levels();
    test_overtemperature_fields_report_warning_and_protection_levels();
    test_under_temperature_fault_fields_report_warning_and_protection_levels();
    test_charge_overcurrent_fault_field_uses_two_levels_and_maps_lock_to_level_one();
    test_discharge_overcurrent_fault_field_maps_protection_levels_and_saturates_at_three();
    test_voltage_difference_field_distinguishes_warning_and_protection();
    test_fault_fields_24_to_26_remain_zero();
    test_fault_field_27_reports_mos_overtemperature_protection_only();
    test_fault_field_28_reports_pf_aggregate_only();
    test_fault_field_29_reports_final_short_circuit_protection_only();
    test_fault_field_31_reports_mos_failure_pf_not_other_hardware_fault();
    test_fault_fields_30_32_34_remain_zero();
    test_detc_presence_starts_standard_can_transmission();
    test_current_threshold_is_inclusive_in_either_direction();
    test_three_minutes_below_threshold_sends_sleep_then_stops();
    test_current_resets_idle_timer_and_restarts_after_stop();
    test_protocol_periods_and_voltage_temperature_cell_data();
    if (failures != 0U)
    {
        printf("%u宗申协议行为测试失败\n", failures);
        return 1;
    }
    puts("PASS zs_b1_can_protocol_behavior_test");
    return 0;
}
