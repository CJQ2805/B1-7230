/* Exercises the real protocol, replacing only hardware and external services.
 * Literal fixtures assert A0.9 wire bytes, not the protocol's encoding helpers. */
#include <stdio.h>
#include <string.h>
#include "../../app/N32G45x_KEIL/User_Src/App/app_bms_status.h"
#include "../../app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c"

struct d_can_t { unsigned unused; };
d_can_t can0;
static app_bms_snapshot_t sample;
static struct _app_bms_fault_t fault_sample;
static t_can_packet frames[3000];
static unsigned frame_count, failures, reset_count;
static bool tx_idle = true;
static bool send_ok = true;
static uint8_t ocv_pending;
static const char *version_v = "V01", *version_t = "T10";
static char sn[51];
static unsigned sn_length;
static uint8_t soc_status = APP_BMS_SOC_WRITE_IDLE;
static uint8_t requested_soc;
static unsigned soc_request_count;
static uint32_t balance_mask;
#define CHECK(x) do { if (!(x)) { printf("FAIL %s:%u: %s\n", __func__, __LINE__, #x); failures++; return; } } while (0)

bool d_can_send(d_can_t *c, uint32_t id, d_can_id_t kind, const uint8_t *d, uint8_t n)
{
    (void)c;
    if (!send_ok || frame_count >= 3000 || n > 8) return false;
    frames[frame_count].u32id = id;
    frames[frame_count].u32idType = kind;
    frames[frame_count].u8len = n;
    memcpy(frames[frame_count++].au8data, d, n);
    return true;
}
bool d_can_is_tx_idle(d_can_t *c) { (void)c; return tx_idle; }
void d_can_set_iap_silence(bool enabled) { (void)enabled; }
void app_bms_snapshot_read(app_bms_snapshot_t *s) { *s = sample; }
void app_bms_fault_snapshot_read(struct _app_bms_fault_t *s) { *s = fault_sample; }
const char *app_param_get_package_v_version(void) { return version_v; }
const char *app_param_get_package_t_version(void) { return version_t; }
const char *app_param_get_hw_version(void) { return "V01"; }
const char *app_param_get_software_pn(void) { return "A000000000"; }
const char *app_param_get_bat_type(void) { return "7230B1"; }
const char *app_param_get_manufacture_sn(void) { return sn; }
bool app_param_set_manufacture_sn(const uint8_t *d, uint8_t n)
{ memcpy(sn, d, n); sn[n] = 0; sn_length = n; return true; }
bool app_param_clear_manufacture_sn(void) { sn[0] = 0; return true; }
bool app_param_set_product_mode(uint8_t mode) { return mode <= APP_PARAM_PRODUCT_MODE_SHIPPING; }
uint16_t app_log_rtd_num(void) { return 2; }
bool ducar_bms_pack_log_payload(uint16_t i, uint8_t d[60])
{ unsigned k; for (k=0;k<60;k++) d[k]=(uint8_t)(k+i); return true; }
bool app_current_calib_get(uint32_t *k, int32_t *b) { *k=1000000; *b=-153; return true; }
bool app_current_calib_submit(int32_t a) { (void)a; return true; }
static uint8_t cal2_fake_status = 3U;
static uint8_t cal2_fake_active;
static uint8_t cal2_fake_request_count;
static app_current_calib_cal2_completion_t cal2_fake_completion;
static uint8_t cal2_fake_completion_pending;
void app_current_calib_cal2_advance_ms(uint32_t ms) { (void)ms; }
bool app_current_calib_cal2_is_active(void) { return cal2_fake_active != 0U; }
bool app_current_calib_cal2_has_completion(void) { return cal2_fake_completion_pending != 0U; }
bool app_current_calib_cal2_take_completion(app_current_calib_cal2_completion_t *out)
{ if (!cal2_fake_completion_pending) return false; *out=cal2_fake_completion; cal2_fake_completion_pending=0; return true; }
uint8_t app_current_calib_cal2_request(uint8_t op, uint8_t seq, uint16_t session,
                                       int32_t reference, uint8_t source)
{ (void)op; (void)seq; (void)session; (void)reference; (void)source; ++cal2_fake_request_count; return cal2_fake_status; }
bool ducar_iap_is_busy(void) { return false; }
bool ducar_can_iap_protocol_is_busy(void) { return false; }
bool d_rtc_read(d_rtc_time_t *t) { memset(t,0,sizeof(*t)); t->year=2026; t->month=10; t->day=12; return true; }
bool d_rtc_read_calendar(d_rtc_time_t *t) { return d_rtc_read(t); }
bool d_rtc_write(const d_rtc_time_t *t) { (void)t; return true; }
uint16_t app_charge_time_get_minutes(void) { return 0U; }
void d_system_reset(void) { reset_count++; }
uint8_t app_bms_fault_save_shutdown_pf(void) { return 1; }
uint8_t app_bms_fault_clear_shutdown_pf(void) { return 1; }
void app_bms_fault_clear_all(void) {}
uint8_t app_hw_apply_clear_all_afe_fault(void) { return 1; }
void app_hw_apply_dcdc_enable(uint8_t e) { (void)e; }
app_result_t app_h_power_manage_set_remote_mos_command(uint8_t m, app_mos_cmd_t c, uint8_t d)
{ (void)m; (void)c; (void)d; return APP_RESULT_OK; }
bool app_bms_force_soc_ocv(void) { return true; }
app_result_t app_power_manage_request_shutdown(void) { return APP_RESULT_OK; }
uint8_t app_bms_soc_ocv_pending(void) { return ocv_pending; }
bool app_bms_soc_write_request(uint8_t s)
{ requested_soc=s; soc_request_count++; soc_status=APP_BMS_SOC_WRITE_SUCCESS; return true; }
uint8_t app_bms_soc_write_status(void) { return soc_status; }
void app_bms_soc_write_complete(void) { soc_status=APP_BMS_SOC_WRITE_IDLE; }
bool app_bms_soc_save_for_transition(void) { return true; }
uint8_t app_bms_soc_ocv_save_status(void) { return APP_BMS_SOC_OCV_SAVED; }
void app_bms_soc_ocv_complete(void) {}
uint32_t app_balance_get_mask(void) { return balance_mask; }

static void fresh(void)
{
    memset(&sample,0,sizeof(sample)); memset(&fault_sample,0,sizeof(fault_sample)); memset(frames,0,sizeof(frames));
    frame_count=reset_count=sn_length=0; sn[0]=0; tx_idle=send_ok=true;
    soc_status=APP_BMS_SOC_WRITE_IDLE; ocv_pending=0;
    requested_soc=0; soc_request_count=0; balance_mask=0;
    version_v="V01"; version_t="T10"; ducar_can_protocol_init();
    cal2_fake_status=3U; cal2_fake_active=0U; cal2_fake_request_count=0U;
    cal2_fake_completion_pending=0U;
}
static void receive(uint32_t id, const uint8_t *d, uint8_t n)
{
    t_can_packet p={0}; p.u32id=id; p.u32idType=D_CAN_ID_EXTENDED; p.u8len=n;
    memcpy(p.au8data,d,n); ducar_can_protocol_receive(&p);
}
static void query(uint16_t pgn)
{
    uint8_t d[3]={(uint8_t)pgn,(uint8_t)(pgn>>8),0};
    receive(0x18EA163A,d,3);
}
static void millisecond(void)
{
    ducar_can_bus_timer_handle(); ducar_can_data_renew(); ducar_can_control_reply_task();
    if (ducar_can_detd_tx_window_is_active()) {
        ducar_can_50ms_task(); ducar_can_100ms_task(); ducar_can_200ms_task();
        ducar_can_500ms_task(); ducar_can_1000ms_task();
    }
}
static void test_version_layout(void)
{
    static const uint8_t expected[8]={1,10,1,9,255,255,255,255};
    fresh(); query(0xF200);
    CHECK(frame_count==1 && frames[0].u32id==0x18F20016 && frames[0].u8len==8);
    CHECK(memcmp(frames[0].au8data,expected,8)==0);
}
static void test_large_minor_version(void)
{
    fresh(); version_t="T4660"; query(0xF200);
    CHECK(frames[0].au8data[1]==255 && frames[0].au8data[2]==1);
    frame_count=0; version_t="T99999999999999999999999"; query(0xF200);
    CHECK(frames[0].au8data[1]==255 && frames[0].au8data[2]==1);
}
static void test_f216_a05_layout(void)
{
    fresh();
    sample.ntc.valid_mask=APP_BMS_NTC_VALID_MOS;
    sample.ntc.mos_temp_0p1c=347;
    sample.temp_valid_mask=(uint8_t)((1U<<0)|(1U<<1));
    sample.temp_0p1c[0]=321;
    sample.temp_0p1c[1]=-45;
    sample.max_temp_num=0; sample.min_temp_num=1;
    sample.cell_count=20; sample.temp_count=2;
    ducar_can_send_f216();
    CHECK(frames[0].u32id==0x18F21616 && frames[0].u8len==8);
    CHECK(frames[0].au8data[0]==34);       /* 34.7 C -> integer 34 C. */
    CHECK(frames[0].au8data[1]==0x80);    /* No pre-MOS source: S8 invalid -128. */
    CHECK(frames[0].au8data[2]==0x41 && frames[0].au8data[3]==0x01);
    CHECK(frames[0].au8data[4]==0xD3 && frames[0].au8data[5]==0xFF);
    CHECK(frames[0].au8data[6]==20 && frames[0].au8data[7]==2);
    fresh(); ducar_can_send_f216();
    CHECK(frames[0].au8data[0]==0x80 && frames[0].au8data[1]==0x80);
    fresh(); sample.ntc.valid_mask=APP_BMS_NTC_VALID_MOS; sample.ntc.mos_temp_0p1c=-129;
    ducar_can_send_f216(); CHECK(frames[0].au8data[0]==0xF4); /* -12.9 C truncates to -12 C. */
}
static void test_f221_balance_mask_layout(void)
{
    fresh(); balance_mask=0x00080021UL;
    ducar_can_send_f221();
    CHECK(frames[0].u32id==0x18F22116 && frames[0].u8len==8);
    CHECK(frames[0].au8data[0]==0x21 && frames[0].au8data[1]==0x00);
    CHECK(frames[0].au8data[2]==0x08 && frames[0].au8data[3]==0x00);
    CHECK(frames[0].au8data[4]==0xFF && frames[0].au8data[7]==0xFF);
}
static void test_f220_controller_uv_and_voltage_event_counts(void)
{
    static const uint8_t normal_temperature_expected[8]={0x58,0x02,0x34,0x12,0x56,0x00,0xFF,0xFF};
    static const uint8_t low_temperature_expected[8]={0x44,0x02,0xFE,0xFF,0xFE,0xFF,0xFF,0xFF};

    fresh();
    sample.temp_count=1U;
    sample.temp_valid_mask=0x01U;
    sample.temp_0p1c[0]=0;
    fault_sample.u32overcharge_count=0x1234UL;
    fault_sample.u32overdischarge_count=0x56UL;
    ducar_can_send_f220();
    CHECK(frames[0].u32id==0x18F22016 && frames[0].u8len==8);
    CHECK(memcmp(frames[0].au8data,normal_temperature_expected,8)==0);

    /* Around 0 C, retain normal mode until the minimum reaches -1.0 C. */
    frame_count=0U;
    sample.temp_0p1c[0]=-1;
    ducar_can_send_f220();
    CHECK(frames[0].au8data[0]==0x58 && frames[0].au8data[1]==0x02);

    /* At -1.0 C switch to the low-temperature setting and saturate counts. */
    frame_count=0U;
    sample.temp_0p1c[0]=-10;
    fault_sample.u32overcharge_count=0xFFFFUL;
    fault_sample.u32overdischarge_count=0x10000UL;
    ducar_can_send_f220();
    CHECK(memcmp(frames[0].au8data,low_temperature_expected,8)==0);

    /* Keep the low-temperature setting through invalid data and the dead band. */
    frame_count=0U;
    sample.temp_count=0U;
    sample.temp_valid_mask=0U;
    ducar_can_send_f220();
    CHECK(frames[0].au8data[0]==0x44 && frames[0].au8data[1]==0x02);

    frame_count=0U;
    sample.temp_count=1U;
    sample.temp_valid_mask=0x01U;
    sample.temp_0p1c[0]=9;
    ducar_can_send_f220();
    CHECK(frames[0].au8data[0]==0x44 && frames[0].au8data[1]==0x02);

    /* Only +1.0 C or above returns to the normal setting. */
    frame_count=0U;
    sample.temp_0p1c[0]=10;
    ducar_can_send_f220();
    CHECK(frames[0].au8data[0]==0x58 && frames[0].au8data[1]==0x02);
}
static void test_periods(void)
{
    static const uint16_t pgn[]={0xF218,0xF20D,0xF20E,0xF219,0xF21A,0xF21B,0xF21C,
        0xF21D,0xF21E,0xF21F,0xF221,0xF20F,0xF210,0xF211,0xF212,0xF213,0xF214,
        0xF215,0xF216,0xF217,0xF220};
    static const unsigned periods[]={50,100,100,100,100,100,100,100,100,100,200,
        200,200,200,200,200,200,500,500,500,1000};
    unsigned last[21]={0},seen[21]={0},t,k,j,before;
    fresh(); sample.signal.input.mcu_detd=1; sample.temp_count=2U;
    for(t=1;t<=4500;t++) {
        before=frame_count; millisecond();
        for(j=before;j<frame_count;j++) for(k=0;k<21;k++)
            if(frames[j].u32id==(0x18000016UL|((uint32_t)pgn[k]<<8))) {
                if(pgn[k]==0xF216U) CHECK(frames[j].au8data[7]==2U);
                if(seen[k]) CHECK(t-last[k]==periods[k]);
                last[k]=t; seen[k]++;
            }
    }
    for(k=0;k<21;k++) CHECK(seen[k]>=4);
}
static void test_heartbeat_window_and_life_value(void)
{
    unsigned t,previous;
    fresh(); query(0xF22F); CHECK(frames[0].au8data[0]==0);
    for(t=0;t<999;t++) millisecond();
    query(0xF22F); CHECK(frames[frame_count-1].au8data[0]==0);
    millisecond(); query(0xF22F); CHECK(frames[frame_count-1].au8data[0]==1);
    /* Last heartbeat at 1000ms: window must last another 5000 calls. */
    for(t=0;t<5000;t++) millisecond();
    previous=frame_count; for(t=0;t<100;t++) millisecond();
    CHECK(frame_count==previous);
    query(0xF22F); previous=frame_count;
    for(t=0;t<100;t++) millisecond();
    CHECK(frame_count>previous);
}
static void test_unmapped_inputs_and_peak_invalid(void)
{
    fresh(); sample.signal.input.mcu_det_load=1; sample.signal.input.mcu_2nd_det=1;
    sample.signal.input.mcu_detd=1; sample.signal.input.mcu_detc=1;
    sample.mos.dsg_mos_state=1; sample.mos.chg_mos_state=1;
    sample.mos.pre_mos_state=1; sample.signal.input.mcu_sw_present=1;
    ducar_can_send_f218(); CHECK(frames[0].au8data[0]==0xF3);
    CHECK(frames[0].au8data[1]==0x3F && frames[0].au8data[2]==0x3F);
    ducar_can_send_f221(); CHECK(frames[1].au8data[0]==0 && frames[1].au8data[3]==0);
    CHECK(frames[1].au8data[4]==255 && frames[1].au8data[7]==255);
}
static void test_f218_reports_bms_work_status_in_data5(void)
{
    static const uint8_t status_inputs[4]={APP_BMS_STATUS_STATIC,APP_BMS_STATUS_DISCHARGE,
        APP_BMS_STATUS_CHARGE,APP_BMS_STATUS_FEEDBACK};
    static const uint8_t expected_data5[4]={0U,1U,2U,3U};
    unsigned i;
    for(i=0U;i<4U;i++) {
        fresh(); sample.bms_work_status=status_inputs[i]; ducar_can_send_f218();
        CHECK(frame_count==1U && frames[0].u32id==0x18F21816UL);
        CHECK(frames[0].au8data[5]==expected_data5[i]);
        CHECK(frames[0].au8data[6]==0U && frames[0].au8data[7]==0U);
    }
}
static void test_protection_levels(void)
{
    fresh(); sample.fault.active.detail.ucharge_fault_A.u16fault_A=(uint16_t)((1U<<8)|(1U<<11));
    ducar_can_send_f21d(); CHECK(frames[0].au8data[1]==1);
    ducar_can_send_f21e(); CHECK(frames[1].au8data[0]==2);

    fresh(); sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[2]==1);
    fresh(); sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[2]==2);
    fresh(); sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1=1;
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[2]==2);
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv=1;
    frame_count=0; ducar_can_send_f21d(); CHECK(frames[0].au8data[2]==3);

    fresh(); sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==1);
    sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2=1;
    frame_count=0; ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==2);
    sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3=1;
    frame_count=0; ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==3);
    sample.fault.active.detail.udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4=1;
    frame_count=0; ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==4);
}

static void test_a06_recharge_warning_and_protection(void)
{
    fresh();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc=1;
    ducar_can_send_f21b(); CHECK(frames[0].u32id==0x18F21B16 && frames[0].au8data[7]==1);
    ducar_can_send_f21f(); CHECK(frames[1].au8data[4]==0);

    fresh();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc=1;
    ducar_can_send_f21b(); CHECK(frames[0].au8data[7]==0);
    ducar_can_send_f21f(); CHECK(frames[1].u32id==0x18F21F16 && frames[1].au8data[4]==1);
}
static void test_tp_tail_padding(void)
{
    static const uint8_t cts[8]={0x11,2,1,255,255,1,0xF2,0};
    unsigned k;
    fresh(); query(0xF201); ducar_can_control_reply_task();
    CHECK(frames[0].u32id==0x18EC3A16 && frames[0].au8data[1]==10 && frames[0].au8data[3]==2);
    receive(0x18EC163A,cts,8); ducar_can_control_reply_task(); ducar_can_control_reply_task();
    CHECK(frame_count==3 && frames[2].u32id==0x18EB3A16 && frames[2].u8len==8);
    CHECK(frames[2].au8data[0]==2 && frames[2].au8data[1]=='0');
    for(k=4;k<8;k++) CHECK(frames[2].au8data[k]==255);
}
static void test_sn_actual_length(void)
{
    static const uint8_t rts[8]={0x10,50,0,8,255,0x30,0xF2,0};
    uint8_t d[8]; unsigned i,j,n=0;
    fresh(); receive(0x18EC163A,rts,8);
    CHECK(frame_count==1 && frames[0].au8data[0]==0x11 && frames[0].au8data[1]==8);
    for(i=1;i<=8;i++) {
        memset(d,255,8); d[0]=(uint8_t)i;
        for(j=1;j<8 && n<50;j++,n++) d[j]=(uint8_t)('A'+n%26);
        receive(0x18EB163A,d,8);
    }
    ducar_can_control_reply_task(); CHECK(sn_length==50);
    for(i=0;i<50;i++) CHECK(sn[i]==(char)('A'+i%26));
    CHECK(frames[1].au8data[0]==0x13 && frames[1].au8data[1]==50);
    CHECK(frames[2].au8data[0]==0 && frames[2].au8data[6]==0x30 && frames[2].au8data[7]==0xF2);
}
static void test_tp_timeout_and_reset_wait(void)
{
    uint8_t reset[8]={1,255,255,255,255,255,255,255}; unsigned t;
    fresh(); query(0xF201); ducar_can_control_reply_task();
    for(t=0;t<4999;t++) ducar_can_bus_timer_handle();
    query(0xF201); ducar_can_control_reply_task();
    CHECK(frames[frame_count-1].u32id==0x18E83A16 && frames[frame_count-1].au8data[0]==1);
    ducar_can_bus_timer_handle(); query(0xF201); ducar_can_control_reply_task();
    CHECK(frames[frame_count-1].u32id==0x18EC3A16);
    fresh(); tx_idle=false; receive(0x1808163A,reset,8); ducar_can_control_reply_task();
    for(t=0;t<99;t++) ducar_can_control_reply_task(); CHECK(reset_count==0);
    ducar_can_control_reply_task(); CHECK(reset_count==1);
}
static void test_log_two_records(void)
{
    uint8_t request[5]={0x32,0xF2,1,2,0};
    uint8_t cts[8]={0x11,9,1,255,255,0x32,0xF2,0};
    uint8_t eom[8]={0x13,60,0,9,255,0x32,0xF2,0}; unsigned i;
    fresh(); receive(0x18EA163A,request,5); ducar_can_control_reply_task();
    receive(0x18EC163A,cts,8); for(i=0;i<9;i++) ducar_can_control_reply_task();
    CHECK(frames[9].u8len==8 && frames[9].au8data[0]==9 && frames[9].au8data[5]==255);
    receive(0x18EC163A,eom,8); ducar_can_control_reply_task();
    CHECK(frames[10].au8data[0]==0x10);
    /* Second record uses two CTS windows: 2 packets, then 7 packets. */
    cts[1]=2; receive(0x18EC163A,cts,8);
    ducar_can_control_reply_task(); ducar_can_control_reply_task();
    CHECK(frames[11].au8data[1]==2);
    cts[1]=7; cts[2]=3; receive(0x18EC163A,cts,8);
    for(i=0;i<7;i++) ducar_can_control_reply_task();
    CHECK(frame_count==20 && frames[19].u8len==8 && frames[19].au8data[0]==9);
    CHECK(frames[19].au8data[1]==58 && frames[19].au8data[4]==61);
    for(i=5;i<8;i++) CHECK(frames[19].au8data[i]==255);
    receive(0x18EC163A,eom,8);
    for(i=0;i<10;i++) ducar_can_control_reply_task();
    CHECK(frame_count==20); /* No third record starts after final EOM. */
    query(0xF201); ducar_can_control_reply_task();
    CHECK(frames[20].u32id==0x18EC3A16 && frames[20].au8data[5]==1);
}
static void test_logical_cells_and_sources(void)
{
    static const uint8_t rated[8]={0xD0,0x02,0x2C,0x01,255,255,255,255};
    fresh(); sample.data_ready=1; sample.cell_count=20; sample.cell_mv[19]=3219;
    ducar_can_send_f213(); CHECK(frames[0].au8data[6]==0x93 && frames[0].au8data[7]==0x0C);
    ducar_can_send_f214(); CHECK(frames[1].au8data[0]==255 && frames[1].au8data[1]==255);
    query(0xF20B); CHECK(memcmp(frames[2].au8data,rated,8)==0);
    query(0xF205); CHECK(frames[3].u8len==5 && memcmp(frames[3].au8data,"DUCAR",5)==0);
}
static void test_control_payload_lengths(void)
{
    static const uint8_t pfs[]={0xE2,0xE3,0xE6,0xE9};
    uint8_t d[8]; unsigned i,n;
    for(i=0;i<4;i++) for(n=0;n<2;n++) {
        fresh(); sample.data_ready=1; memset(d,255,8);
        d[0]=(i==0)?50:((i==3)?1:0); if(i==0) d[1]=0;
        receive(0x1800163AUL|((uint32_t)pfs[i]<<16),d,(uint8_t)(n?8:((i==0)?2:1)));
        ducar_can_control_reply_task(); ducar_can_control_reply_task();
        CHECK(frame_count==1 && frames[0].u32id==0x18E83A16 && frames[0].au8data[0]==0);
        CHECK(frames[0].au8data[6]==0 && frames[0].au8data[7]==pfs[i]);
    }
}
static void test_address_validation_and_silent_window(void)
{
    const uint8_t heartbeat[3]={0x2F,0xF2,0};
    const uint8_t invalid[3]={0x2F,0xF2,1};
    uint8_t bus[8]={1,255,255,255,255,255,255,255}; unsigned t,before;
    fresh(); receive(0x18EA1616,heartbeat,3); CHECK(frame_count==0);
    receive(0x18EA173A,heartbeat,3); CHECK(frame_count==0);
    receive(0x18EA163A,invalid,3); ducar_can_control_reply_task();
    CHECK(frame_count==1 && frames[0].au8data[0]==1);
    before=frame_count; for(t=0;t<100;t++) millisecond(); CHECK(frame_count==before);
    receive(0x18EAFF3A,heartbeat,3); CHECK(frames[frame_count-1].u32id==0x18F22F16);
    receive(0x18E0163A,bus,8); before=frame_count;
    for(t=0;t<5001;t++) millisecond(); CHECK(frame_count==before);
    bus[0]=2; receive(0x18E0163A,bus,8);
    for(t=0;t<100;t++) millisecond(); CHECK(frame_count==before);
    query(0xF22F); before=frame_count;
    for(t=0;t<100;t++) millisecond(); CHECK(frame_count>before);
}
static void test_any_valid_pc_query_refreshes_tx_window(void)
{
    unsigned t, before;
    const uint8_t unsupported_query[3]={0x03,0xF2,0};

    fresh(); query(0xF22F);
    for(t=0;t<4800;t++) millisecond();
    /* A valid query for an unsupported PGN still refreshes before its NAK. */
    query(0xF203); ducar_can_control_reply_task();
    for(t=0;t<200;t++) millisecond();
    before=frame_count;
    for(t=0;t<50;t++) millisecond();
    CHECK(frame_count>before);

    fresh(); query(0xF22F);
    for(t=0;t<4800;t++) millisecond();
    /* A PC request addressed to another BMS must not refresh this unit. */
    receive(0x18EA173A,unsupported_query,sizeof(unsupported_query));
    for(t=0;t<200;t++) millisecond();
    before=frame_count;
    for(t=0;t<50;t++) millisecond();
    CHECK(frame_count==before);
}
static void test_life_value_wrap(void)
{
    unsigned t;
    fresh(); for(t=0;t<255000;t++) ducar_can_bus_timer_handle();
    query(0xF22F); CHECK(frames[0].au8data[0]==255);
    for(t=0;t<999;t++) ducar_can_bus_timer_handle();
    query(0xF22F); CHECK(frames[frame_count-1].au8data[0]==255);
    ducar_can_bus_timer_handle();
    query(0xF22F); CHECK(frames[frame_count-1].au8data[0]==0);
}
static void test_rejection_and_sn_bounds(void)
{
    uint8_t rts[8]={0x10,51,0,8,255,0x30,0xF2,0};
    const uint8_t fields[8]={0,0,0,0,0,0,255,255}; unsigned i;
    fresh(); receive(0x18EC163A,rts,8); ducar_can_control_reply_task();
    CHECK(sn_length==0 && frames[0].au8data[0]==1);
    for(i=0;i<3;i++) {
        static const uint16_t unsupported[]={0xF203,0xF20C,0xF22E};
        fresh(); query(unsupported[i]); ducar_can_control_reply_task();
        CHECK(frame_count==1 && frames[0].u32id==0x18E83A16 && frames[0].au8data[0]==1);
    }
    for(i=0;i<3;i++) {
        fresh(); receive(0x180A163AUL+(i<<16),fields,8); ducar_can_control_reply_task();
        CHECK(frame_count==1 && frames[0].u32id==0x18E83A16 && frames[0].au8data[0]==1);
        CHECK(frames[0].au8data[6]==0 && frames[0].au8data[7]==0x0A+i);
    }
}
static void test_native_discharge_fault_bits(void)
{
    fresh(); sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1low_soc=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==0);
    ducar_can_send_f21c(); CHECK(frames[1].au8data[1]==1);
    ducar_can_send_f21f(); CHECK(frames[2].au8data[1]==1);
    fresh(); sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock=1;
    ducar_can_send_f21d(); CHECK(frames[0].au8data[7]==0);
    ducar_can_send_f21c(); CHECK(frames[1].au8data[1]==0);
}
static void test_f21f_overcurrent_lock_bits(void)
{
    fresh();
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock=1;
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock=1;
    ducar_can_send_f21f();
    CHECK(frames[0].u32id==0x18F21F16);
    CHECK(frames[0].au8data[4]==0);
    CHECK(frames[0].au8data[5]==0 && frames[0].au8data[6]==0x0C && frames[0].au8data[7]==0);

    /* Assert each source independently so swapped BIT2/BIT3 wiring is detected. */
    fresh();
    sample.fault.active.detail.ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock=1;
    ducar_can_send_f21f();
    CHECK(frames[0].au8data[5]==0 && frames[0].au8data[6]==0x04);

    fresh();
    sample.fault.active.detail.udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock=1;
    ducar_can_send_f21f();
    CHECK(frames[0].au8data[5]==0 && frames[0].au8data[6]==0x08);

    fresh();
    sample.fault.active.detail.ucharge_fault_A.tcharge_fault_A.b1full_charge_flag=1;
    ducar_can_send_f21f();
    CHECK(frames[0].au8data[5]==1 && frames[0].au8data[6]==0);
}
static void test_e200_single_byte_soc(void)
{
    const uint8_t soc75[1]={0x4B};
    const uint8_t soc75_with_reserved[8]={0x4B,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    const uint8_t soc101[1]={101};
    const uint8_t empty_payload[1]={0};
    fresh(); sample.data_ready=1; sample.max_cell_mv=3000U;
    receive(0x18E2163A,soc75,sizeof(soc75));
    CHECK(soc_request_count==1 && requested_soc==75);
    ducar_can_control_reply_task(); ducar_can_control_reply_task();
    CHECK(frames[0].u32id==0x18E83A16 && frames[0].au8data[0]==0);
    CHECK(frames[0].au8data[6]==0 && frames[0].au8data[7]==0xE2);
    for(unsigned i=1;i<6;i++) CHECK(frames[0].au8data[i]==0xFF);
    fresh(); receive(0x18E2163A,soc75_with_reserved,sizeof(soc75_with_reserved));
    CHECK(soc_request_count==1 && requested_soc==75); /* Data1~Data7 are ignored/reserved. */
    fresh(); receive(0x18E2163A,soc101,sizeof(soc101));
    ducar_can_control_reply_task();
    CHECK(soc_request_count==0 && frames[0].u32id==0x18E83A16);
    CHECK(frames[0].au8data[0]==1 && frames[0].au8data[6]==0 && frames[0].au8data[7]==0xE2);
    for(unsigned i=1;i<6;i++) CHECK(frames[0].au8data[i]==0xFF);

    fresh(); receive(0x18E2163A,empty_payload,0);
    ducar_can_control_reply_task();
    CHECK(soc_request_count==0 && frames[0].u32id==0x18E83A16);
    CHECK(frames[0].au8data[0]==1 && frames[0].au8data[6]==0 && frames[0].au8data[7]==0xE2);
}
static void test_ocv_reply_survives_busy_ack(void)
{
    const uint8_t clear[1]={0},ocv[1]={1}; unsigned t;
    fresh(); sample.data_ready=1;
    receive(0x18E6163A,clear,1); receive(0x18E9163A,ocv,1);
    for(t=0;t<4;t++) ducar_can_control_reply_task();
    CHECK(frame_count==2 && frames[0].au8data[7]==0xE6 && frames[1].au8data[7]==0xE9);
    CHECK(frames[1].au8data[0]==0);
    /* Timeout result must remain NAK even if OCV completes while the ACK slot is busy. */
    fresh(); sample.data_ready=1; ocv_pending=1; receive(0x18E9163A,ocv,1);
    for(t=0;t<4999;t++) ducar_can_control_reply_task();
    receive(0x18E6163A,clear,1); ducar_can_control_reply_task(); ocv_pending=0;
    for(t=0;t<3;t++) ducar_can_control_reply_task();
    CHECK(frame_count==2 && frames[1].au8data[7]==0xE9 && frames[1].au8data[0]==1);
}
static void test_tp_send_stall_releases_session(void)
{
    const uint8_t cts[8]={0x11,2,1,255,255,1,0xF2,0}; unsigned stage,t;
    for(stage=0;stage<2;stage++) {
        fresh(); query(0xF201);
        if(stage) { ducar_can_control_reply_task(); receive(0x18EC163A,cts,8); }
        send_ok=false;
        for(t=0;t<5000;t++) { ducar_can_bus_timer_handle(); ducar_can_control_reply_task(); }
        send_ok=true; frame_count=0; query(0xF201); ducar_can_control_reply_task();
        CHECK(frame_count==1 && frames[0].u32id==0x18EC3A16 && frames[0].au8data[0]==0x10);
    }
}
static void test_cal2_wire_contract(void)
{
    const uint8_t begin[8]={0,0,0,0,1,1,0x34,0x12};
    const uint8_t expected[8]={3,1,1,0x34,0x12,0,0xE7,0};
    fresh(); query(DUCAR_CAN_PGN_F233);
    CHECK(frame_count==1 && frames[0].u32id==0x18F23316 && frames[0].u8len==8);
    CHECK(memcmp(frames[0].au8data,"CAL2\x01\x01\xFF\xFF",8)==0);
    fresh(); receive(0x18E7163A,begin,8); ducar_can_control_reply_task();
    CHECK(cal2_fake_request_count==1 && frame_count==1);
    CHECK(frames[0].u32id==0x18E83A16 && frames[0].u8len==8);
    CHECK(memcmp(frames[0].au8data,expected,8)==0);
    fresh(); receive(0x18E7163A,begin,4); ducar_can_control_reply_task();
    CHECK(cal2_fake_request_count==0 && frame_count==1);
    CHECK(frames[0].au8data[1]==0xFF && frames[0].au8data[6]==0 && frames[0].au8data[7]==0xE7);
}
int main(int argc, char **argv)
{
    /* Focused protocol check while older A0.7 fixtures are being updated. */
    if ((argc == 2) && (strcmp(argv[1], "f21f") == 0))
    {
        test_f21f_overcurrent_lock_bits();
        return failures ? 1 : 0;
    }
    test_version_layout(); test_large_minor_version(); test_f216_a05_layout();
    test_f221_balance_mask_layout(); test_f220_controller_uv_and_voltage_event_counts(); test_periods();
    test_heartbeat_window_and_life_value(); test_unmapped_inputs_and_peak_invalid();
    test_f218_reports_bms_work_status_in_data5();
    test_protection_levels(); test_a06_recharge_warning_and_protection();
    test_tp_tail_padding(); test_sn_actual_length();
    test_tp_timeout_and_reset_wait(); test_log_two_records(); test_logical_cells_and_sources();
    test_control_payload_lengths();
    test_address_validation_and_silent_window();
    test_any_valid_pc_query_refreshes_tx_window(); test_life_value_wrap();
    test_rejection_and_sn_bounds();
    test_native_discharge_fault_bits(); test_ocv_reply_survives_busy_ack();
    test_f21f_overcurrent_lock_bits(); test_e200_single_byte_soc();
    test_tp_send_stall_releases_session();
    test_cal2_wire_contract();
    printf("CAN A0.7 behavior: %s (%u failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
