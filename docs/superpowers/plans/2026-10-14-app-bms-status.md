# BMS Operating Status Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the requested four-state BMS operating status from debounced DETC/DETD and processed current.

**Architecture:** A small App module keeps qualification history private and exposes only `gu8AppBmsStatus` plus its periodic update entry point. `DataHandle_Task` calls it after refreshing the consistent BMS snapshot.

**Tech Stack:** C99 firmware, Keil uVision project, existing PowerShell/MSVC host-test harness.

**Spec:** `docs/superpowers/specs/2026-10-14-app-bms-status-design.md`

## Global Constraints

- Reuse processed `current_10ma`; do not add another current-presence threshold.
- DETC has top priority and immediately selects status 2.
- DETC absent + positive current starts in status 3; 5 seconds with peak-to-peak <=500mA (50 `current_10ma` counts) selects status 2.
- DETD is evaluated only when there is no positive current; negative current or DETD present selects status 1, otherwise status 0.
- Zero/negative current cancels charge qualification immediately.
- Preserve all pre-existing worktree changes; modify only listed files.

## Review Focus

- The exact 500mA boundary must remain stable; test the inclusive comparison.
- A one-sample >500mA excursion in status 2 must immediately return to feedback and require a fresh 5-second qualification.
- Qualification uses elapsed kernel ticks, not loop count; a sampling gap over 100ms restarts continuous-stability qualification.
- DETC insertion must override zero or negative current and DETD.
- Current becoming zero/negative during qualification must discard prior stability history.
- Status update cadence must follow the refreshed 10ms snapshot, not duplicate raw input debouncing.

---

### Task 1: Add and integrate the BMS status machine

**Files:**
- Create: `app/N32G45x_KEIL/User_Src/App/app_bms_status.h`
- Create: `app/N32G45x_KEIL/User_Src/App/app_bms_status.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/data_handle_task.c`
- Modify: `app/N32G45x_KEIL/MDK-ARM/N32G45x.uvprojx`
- Create: `tool/tests/app_bms_status_behavior_test.c`
- Create: `tool/tests/host_stubs/bms_status/App/app_bms_snapshot.h`
- Create: `tool/tests/run_app_bms_status_behavior_test.ps1`

**Interfaces:**
- Consumes: `app_bms_snapshot_read(app_bms_snapshot_t *)`, with `current_10ma`, `signal.input.mcu_detc`, and `signal.input.mcu_detd`.
- Produces: `volatile uint8_t gu8AppBmsStatus`; constants `APP_BMS_STATUS_STATIC`, `APP_BMS_STATUS_DISCHARGE`, `APP_BMS_STATUS_CHARGE`, `APP_BMS_STATUS_FEEDBACK`; periodic `app_bms_status_task(void)`; no init function.

- [x] **Step 1: Write failing behavior tests** for status priority, no-current DETD handling, positive-current feedback, 500mA inclusive range, elapsed 5-second qualification with nonuniform samples and tick wrap, sampling-gap reset, >500mA fallback/requalification, and current-sign interruption.
- [x] **Step 2: Run the test** with `powershell -NoProfile -ExecutionPolicy Bypass -File tool/tests/run_app_bms_status_behavior_test.ps1`.
  Expected: compilation/link fails only because the new status module/interface does not yet exist.
- [x] **Step 3: Implement the module** with private rolling-window state based on `osKernelSysTick()` and the public one-byte output; call it immediately after `app_bms_snapshot_update()` in `DataHandle_Task`; add its C source to the Keil APP group.
- [x] **Step 4: Re-run the focused test** with the same PowerShell command.
  Expected: all BMS status behavior assertions pass.
- [x] **Step 5: Verify project integration** by running existing tests and a Keil APP build. The Keil build succeeds; 32 of 33 PowerShell tests pass, with `uds_flash_driver_artifact_test.ps1` unable to launch its configured `python.exe` in this environment.

