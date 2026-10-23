#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "App/app_h_power_manage.h"
#include "App/app_bms_fault.h"
#include "App/app_bms_snapshot.h"
#include "Board/board_power_port.h"

/* Execute the real MOS state machine; only the hardware/time boundary is stubbed. */
#include "App/app_h_power_manage.c"

static uint32_t s_now_ms;
static uint8_t s_applied_cmos;
static uint8_t s_applied_dmos;
static uint8_t s_applied_pdmos;
static app_bms_snapshot_t s_task_snapshot;

static app_result_t set_cmos(uint8_t value) { s_applied_cmos = value; return APP_RESULT_OK; }
static app_result_t set_dmos(uint8_t value) { s_applied_dmos = value; return APP_RESULT_OK; }
static app_result_t set_pdmos(uint8_t value) { s_applied_pdmos = value; return APP_RESULT_OK; }
static app_result_t set_all(uint8_t cmos, uint8_t dmos, uint8_t pdmos)
{
    s_applied_cmos = cmos;
    s_applied_dmos = dmos;
    s_applied_pdmos = pdmos;
    return APP_RESULT_OK;
}
static uint32_t get_time_ms(void) { return s_now_ms; }

app_h_power_manage_port_t app_board_power_port_get(void)
{
    app_h_power_manage_port_t port;
    memset(&port, 0, sizeof(port));
    port.set_charge_mos = set_cmos;
    port.set_discharge_mos = set_dmos;
    port.set_precharge_mos = set_pdmos;
    port.set_all_mos = set_all;
    port.get_time_ms = get_time_ms;
    return port;
}

void app_bms_snapshot_read(app_bms_snapshot_t *snapshot) { *snapshot = s_task_snapshot; }
void app_bms_fault_snapshot_read(struct _app_bms_fault_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
}
uint8_t app_bms_fault_get_efuse_cmd(void) { return 0U; }

static int sample(app_bms_snapshot_t *snapshot, app_h_power_manage_request_t *request,
                  uint32_t time_ms, uint8_t want_dmos, uint8_t want_pdmos,
                  const char *case_name)
{
    app_result_t result;
    s_now_ms = time_ms;
    result = app_h_power_manage_process(&gt_app_h_power_manage, snapshot, request);
    if ((result != APP_RESULT_OK) ||
        (gt_app_h_power_manage.tcmd.u8dmos_ctl != want_dmos) ||
        (gt_app_h_power_manage.tcmd.u8pdmos_ctl != want_pdmos) ||
        (s_applied_dmos != want_dmos) || (s_applied_pdmos != want_pdmos))
    {
        fprintf(stderr, "FAIL %s at %lu ms: result=%d DMOS=%u PDMOS=%u, want %u/%u\n",
                case_name, (unsigned long)time_ms, result,
                gt_app_h_power_manage.tcmd.u8dmos_ctl,
                gt_app_h_power_manage.tcmd.u8pdmos_ctl,
                want_dmos, want_pdmos);
        return 1;
    }
    return 0;
}

int main(void)
{
    app_bms_snapshot_t snapshot;
    app_h_power_manage_request_t request;
    app_h_power_manage_output_t sleep_output;

    memset(&snapshot, 0, sizeof(snapshot));
    s_now_ms = 0U;
    if (app_h_power_manage_init() != APP_RESULT_OK)
    {
        fputs("FAIL power manager init\n", stderr);
        return 1;
    }
    request = gt_app_h_power_manage.tctl;
    snapshot.data_ready = 1U;
    snapshot.signal.input.mcu_detd = 0U;
    if (sample(&snapshot, &request, 10U, 0U, 0U,
               "DETD absent must hold both discharge MOS paths off") != 0)
    {
        return 1;
    }

    snapshot.signal.input.mcu_detd = 1U;
    if (sample(&snapshot, &request, 20U, 0U, 1U, "precharge begins on DETD insertion") != 0 ||
        sample(&snapshot, &request, 319U, 0U, 1U, "DMOS waits for full 300 ms") != 0 ||
        sample(&snapshot, &request, 320U, 1U, 1U, "DMOS opens after precharge") != 0)
    {
        return 1;
    }
    snapshot.mos.dsg_mos_state = 1U;
    if (sample(&snapshot, &request, 330U, 1U, 0U, "PDMOS releases after DMOS feedback") != 0)
    {
        return 1;
    }

    request.u8dmos_fault_forbid = 1U;
    if (sample(&snapshot, &request, 340U, 0U, 0U, "fault closes DMOS and PDMOS") != 0)
    {
        return 1;
    }
    request.u8dmos_fault_forbid = 0U;
    snapshot.mos.dsg_mos_state = 0U;
    if (sample(&snapshot, &request, 350U, 0U, 1U, "fault recovery restarts precharge") != 0 ||
        sample(&snapshot, &request, 649U, 0U, 1U, "fault recovery waits full 300 ms") != 0 ||
        sample(&snapshot, &request, 650U, 1U, 1U, "DMOS reopens after new precharge") != 0)
    {
        return 1;
    }
    snapshot.mos.dsg_mos_state = 1U;
    if (sample(&snapshot, &request, 660U, 1U, 0U, "second closure releases PDMOS") != 0)
    {
        return 1;
    }

    request.i8dmos_ctl_force[MF_Remote] = APP_MOS_OFF;
    if (sample(&snapshot, &request, 670U, 0U, 0U, "remote close stops DMOS") != 0)
    {
        return 1;
    }
    request.i8dmos_ctl_force[MF_Remote] = APP_MOS_ON;
    snapshot.mos.dsg_mos_state = 0U;
    if (sample(&snapshot, &request, 680U, 0U, 1U, "remote reopen must restart precharge") != 0 ||
        sample(&snapshot, &request, 979U, 0U, 1U, "remote reopen waits full 300 ms") != 0 ||
        sample(&snapshot, &request, 980U, 1U, 1U, "remote reopen after precharge") != 0)
    {
        return 1;
    }
    snapshot.signal.input.mcu_detd = 0U;
    if (sample(&snapshot, &request, 990U, 0U, 0U, "forced on cannot bypass absent DETD") != 0)
    {
        return 1;
    }

    request.i8dmos_ctl_force[MF_Remote] = APP_MOS_IGNORE;
    snapshot.signal.input.mcu_detd = 1U;
    if (sample(&snapshot, &request, 1000U, 0U, 1U, "new insertion starts precharge") != 0 ||
        sample(&snapshot, &request, 1300U, 1U, 1U, "new insertion completes precharge") != 0)
    {
        return 1;
    }
    snapshot.mos.dsg_mos_state = 1U;
    if (sample(&snapshot, &request, 1310U, 1U, 0U, "hardware feedback confirms DMOS") != 0)
    {
        return 1;
    }
    snapshot.mos.dsg_mos_state = 0U;
    if (sample(&snapshot, &request, 1320U, 0U, 0U,
               "unexpected hardware DMOS drop must cancel prior precharge") != 0 ||
        sample(&snapshot, &request, 1330U, 0U, 1U,
               "hardware recovery starts new precharge") != 0 ||
        sample(&snapshot, &request, 1629U, 0U, 1U,
               "hardware recovery waits full 300 ms") != 0 ||
        sample(&snapshot, &request, 1630U, 1U, 1U,
               "hardware recovery reopens DMOS after precharge") != 0)
    {
        return 1;
    }

    if (app_h_power_manage_init() != APP_RESULT_OK)
    {
        fputs("FAIL power manager reinit\n", stderr);
        return 1;
    }
    request = gt_app_h_power_manage.tctl;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.data_ready = 1U;
    snapshot.signal.input.mcu_detd = 1U;
    if (sample(&snapshot, &request, 2000U, 0U, 1U, "precharge starts before interruption") != 0)
    {
        return 1;
    }
    request.i8pdmos_ctl_force[MF_Calib] = APP_MOS_OFF;
    if (sample(&snapshot, &request, 2100U, 0U, 0U, "precharge MOS forced off interrupts timer") != 0 ||
        sample(&snapshot, &request, 2400U, 0U, 0U, "interrupted precharge cannot age into completion") != 0)
    {
        return 1;
    }
    request.i8pdmos_ctl_force[MF_Calib] = APP_MOS_IGNORE;
    if (sample(&snapshot, &request, 2410U, 0U, 1U, "precharge restarts after interruption") != 0 ||
        sample(&snapshot, &request, 2709U, 0U, 1U, "interruption requires full new 300 ms") != 0 ||
        sample(&snapshot, &request, 2710U, 1U, 1U, "DMOS opens after restarted precharge") != 0)
    {
        return 1;
    }
    snapshot.mos.dsg_mos_state = 1U;
    if (sample(&snapshot, &request, 2720U, 1U, 0U, "DMOS feedback closes before sleep") != 0)
    {
        return 1;
    }
    app_h_power_manage_prepare_sleep(&sleep_output);
    s_task_snapshot = snapshot;
    s_task_snapshot.mos.dsg_mos_state = 0U;
    s_now_ms = 2730U;
    if ((app_h_power_manage_restore_after_wakeup(&sleep_output) != APP_RESULT_OK) ||
        (gt_app_h_power_manage.tcmd.u8dmos_ctl != 0U) ||
        (gt_app_h_power_manage.tcmd.u8pdmos_ctl != 0U) ||
        (s_applied_dmos != 0U))
    {
        fprintf(stderr, "FAIL wakeup must restart precharge: command=%u/%u applied=%u/%u "
                        "feedback=%u last=%d step=%u\n",
                gt_app_h_power_manage.tcmd.u8dmos_ctl,
                gt_app_h_power_manage.tcmd.u8pdmos_ctl,
                s_applied_dmos, s_applied_pdmos,
                gt_app_h_power_manage.tstat.u8dsgmos_state,
                gt_app_h_power_manage.last_dmos_cmd,
                gt_app_h_power_manage.tpdsg.u8step);
        return 1;
    }
    snapshot.mos.dsg_mos_state = 0U;
    if (sample(&snapshot, &request, 2740U, 0U, 1U, "wakeup starts fresh precharge") != 0 ||
        sample(&snapshot, &request, 3039U, 0U, 1U, "wakeup waits full precharge delay") != 0 ||
        sample(&snapshot, &request, 3040U, 1U, 1U, "wakeup reopens DMOS after precharge") != 0)
    {
        return 1;
    }

    puts("PASS: DETD/precharge discharge MOS behavior");
    return 0;
}
