# N32 Soft Shutdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Implement the approved N32 soft shutdown sequence, including a 15-second delay, MOS-off confirmation, `power_off_f` realtime LOG records, AFE/ADC quiescence, and a 3-second reset fallback.

**Architecture:** CAN and UART submit an idempotent request only after their ACK has completed. A BMS-task state machine uses `d_timer_get_ms()` without blocking other tasks, coordinates the log queue and MOS manager, then shuts down AFE, ADC, and PC4 `DCDC_EN` in order.

**Tech Stack:** C, N32G45x Standard Peripheral Library, CMSIS-RTOS/FreeRTOS, Keil MDK.

**Spec:** `docs/superpowers/specs/2026-10-13-n32-soft-shutdown-design.md`

## Global Constraints

- Start the 15-second timer only after the CAN/UART ACK transmission completes.
- Use PC4 `DCDC_EN` as the confirmed 12V enable.
- Confirm charge/discharge MOS off using valid AFE feedback; confirm precharge off using its software output mirror.
- Do not enter AFE SHUTDOWN or disable ADC/12V until all three MOS states are confirmed off.
- Set the existing `log_sw_err_t.bit.power_off_f` bit in shutdown records; keep `log_rtd_t` at 70 bytes and preserve the Flash slot format.
- Stop AFE sampling, current calibration, and balancing after AFE SHUTDOWN; prevent ADC reads from reinitializing ADC until MCU reset.
- Wait 3 seconds after pulling down `DCDC_EN`, then call `d_system_reset()` if code is still running.
- Preserve unrelated pre-existing changes in the working tree; do not touch AFE topology mapping files.

## Review Focus

- Duplicate shutdown commands must not restart the 15-second timer. Owned by Tasks 4–5.
- `MF_Poweroff` must override calibration and remote MOS-on requests. Owned by Task 2.
- Invalid or stale AFE status must not be treated as MOS-off; precharge uses only the approved output mirror. Owned by Task 4.
- Log storage unavailable or a full queue must not trap the controller forever; shutdown continues after the 2-second final-log wait. Owned by Tasks 1 and 4.
- AFE/ADC service code must not restart after its shutdown latch is set. Owned by Task 3.

---

### Task 1: Add shutdown event logging and flush status

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/App/app_log_manage.h`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_log_manage.c`

**Interfaces:**
- Produce `LOG_TRIGGER_SOFT_SHUTDOWN = 6` in `log_trigger_t`.
- Produce `bool app_log_request_snapshot(log_trigger_t trigger)`: return `false` if LOG storage is unavailable; return `true` when the snapshot is queued.
- Produce `app_log_flush_status_t app_log_get_flush_status(void)` with `APP_LOG_FLUSH_COMPLETE`, `APP_LOG_FLUSH_PENDING`, and `APP_LOG_FLUSH_FAILED`.

- [x] Add the shutdown trigger and flush status enum without changing `log_rtd_t` layout or size.
- [x] Update `app_log_request_snapshot()` to report whether the logger accepted the request; existing callers may ignore the result.
- [x] In snapshot construction, set `log->sw_err.bit.power_off_f = 1U` only for `LOG_TRIGGER_SOFT_SHUTDOWN`.
- [x] Implement flush status from `s_log_ready` and `s_log_pending_count`: failed if storage is unavailable, complete if the queue is empty, otherwise pending.
- [x] Review the LOG field assignment and confirm the record still uses the existing `sw_err.status` wire/storage field.

### Task 2: Add shutdown MOS override and sleep inhibit

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/App/app_h_power_manage.h`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_h_power_manage.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_low_power_manage.h`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_low_power_manage.c`

**Interfaces:**
- Produce `void app_h_power_manage_set_poweroff_override(uint8_t enable)`; enabling sets all three `MF_Poweroff` commands to `APP_MOS_OFF`, disabling sets them to `APP_MOS_IGNORE`.
- Produce `void app_low_power_set_shutdown_inhibit(uint8_t inhibit)`; while inhibited, `app_low_power_is_sleep_allowed()` returns false.

- [x] Move `MF_Poweroff` to priority index 0 so it wins `app_h_power_manage_judge_force()` over calibration, remote, and ID-assignment requests.
- [x] Implement the three-output setter and ensure its command persists through `app_h_power_manage_task()` request rebuilding.
- [x] Add the low-power inhibit latch and check it before all other sleep-eligibility conditions.
- [x] Review that ordinary low-power behavior is unchanged when the inhibit is clear.

### Task 3: Quiesce AFE and ADC after shutdown

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.h`
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/afe_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/Driver/d_adc.h`
- Modify: `app/N32G45x_KEIL/User_Src/Driver/d_adc.c`

**Interfaces:**
- Produce `void d_adc_deinit(void)`; it disables ADC1/ADC2, the internal temperature sensor/VREFINT, and ADC clocks, and latches ADC off until reset.
- Add a volatile shutdown flag to `hsi_afe_t`; `hsi_afe_shutdown()` sets it while holding the AFE resource lock after issuing SHUTDOWN.

- [x] Add the ADC shutdown latch; make `d_adc_init()` a no-op and `d_adc_read()` return false after `d_adc_deinit()` until MCU reset.
- [x] Implement `d_adc_deinit()` using the N32 ADC and RCC APIs already used by `d_adc_init()`.
- [x] Set the AFE shutdown flag inside `hsi_afe_shutdown()` while holding the existing resource lock.
- [x] In `AFE_Task`, skip `hsi_afe_task()`, `app_current_calib_task()`, and `app_balance_task()` once the AFE shutdown flag is set.
- [x] Review lock ordering so AFE shutdown waits for an active AFE cycle and no later cycle accesses DVC1124.

### Task 4: Implement the shutdown state machine

**Files:**
- Create: `app/N32G45x_KEIL/User_Src/App/app_power_manage.h`
- Create: `app/N32G45x_KEIL/User_Src/App/app_power_manage.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/bms_task.c`
- Modify: `app/N32G45x_KEIL/Core/src/main.c`
- Modify: `app/N32G45x_KEIL/MDK-ARM/N32G45x.uvprojx`

**Interfaces:**
- Produce `void app_power_manage_init(void)`.
- Produce `app_result_t app_power_manage_request_shutdown(void)`; repeated requests while active are idempotent and do not reset the timer.
- Produce `void app_power_manage_task(void)`; call it before `app_h_power_manage_task()` in each BMS iteration so a newly asserted MOS override is applied in the same cycle.

- [x] Define states `IDLE`, `WAIT_15S`, `WAIT_MOS_OFF`, `WAIT_FINAL_LOG`, `WAIT_POWER_DOWN`, and `COMPLETE`.
- [x] On the first request, inhibit sleep, request `LOG_TRIGGER_SOFT_SHUTDOWN`, record `d_timer_get_ms()`, and enter `WAIT_15S`.
- [x] After `15000U` ms, set the MOS power-off override and enter `WAIT_MOS_OFF`.
- [x] In `WAIT_MOS_OFF`, read `app_bms_snapshot_t`; proceed only when `afe_status_valid != 0U`, `chg_mos_state == 0U`, `dsg_mos_state == 0U`, and `pre_mos_state == 0U`.
- [x] Request the final shutdown snapshot and remain in `WAIT_FINAL_LOG` until flush is complete, storage fails, or `2000U` ms elapses.
- [x] Call `hsi_afe_shutdown()`, then `d_adc_deinit()`, then `app_hw_apply_dcdc_enable(0U)`; record the power-down timestamp only after lowering PC4.
- [x] After `3000U` ms in `WAIT_POWER_DOWN`, call `d_system_reset()`; keep the state terminal if reset unexpectedly returns.
- [x] Initialize the module in `main.c`, call its task from `BMS_Task`, and register the new source in the Keil project.
- [x] Review every state transition against the 15-second, MOS feedback, log flush, AFE/ADC shutdown, and 3-second fallback requirements.

### Task 5: Route CAN and UART commands through the manager

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c`
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_uart_protocol.c`

**Interfaces:**
- Consume `app_result_t app_power_manage_request_shutdown(void)` from Task 4.

- [x] Replace the CAN `DUCAR_CAN_ACTION_POWER_OFF` direct DCDC write with a shutdown-manager request after ACK transmission completes.
- [x] Replace the UART shutdown handler's direct DCDC write with a shutdown-manager request after `d_uart_wait_tx_complete()` succeeds.
- [x] Preserve command validation and the existing pre-ACK PF persistence / ACK behavior.
- [x] Review that no protocol shutdown call site directly lowers `DCDC_EN`.

## Implementation notes

- The current `SaveLog_Task` waits 1000 ms between calls, despite older comments describing a 10 ms period; this plan uses a 2000 ms bounded wait for the final snapshot.
- This plan does not add or run tests. The implementation should preserve the existing topology files and all unrelated dirty working-tree changes.

