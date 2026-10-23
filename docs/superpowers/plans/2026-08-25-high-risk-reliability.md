# High-Risk Reliability Items Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ensure APP safety/protection work cannot be delayed or corrupted by communication, logging, Flash access, shared AFE data, or stalled task execution.

**Architecture:** Keep AFE sampling at `osPriorityRealtime`; raise BMS protection above communication and logging. Add explicit recursive resource ownership for the shared Flash and AFE hardware/data paths, while keeping short snapshot copies bounded. Extend the existing global FreeRTOS monitor with per-task deadline miss counters so a task that runs but misses its required period is not considered healthy.

**Tech Stack:** N32G452REL7, FreeRTOS/CMSIS-RTOS v1 compatibility layer, N32G45x standard peripheral library, Keil MDK-ARM.

**Spec:** The approved high-risk list in the 2026-08-25 conversation: BMS protection priority, Flash arbitration, shared AFE/BMS data consistency, and high-priority task execution timeout monitoring.

## Global Constraints

- Modify APP only; do not modify BOOT.
- Preserve existing public business interfaces unless a new explicit status/guard interface is required.
- Do not restore scattered generic `lock/unlock` calls; use named resource guards with bounded ownership.
- Do not add blocking Flash or I2C operations to an ISR.
- Keep the existing `osDelay` task timing model and TIM7 1ms SOC timing.
- Every production behavior change requires a failing static regression test before implementation.
- Final Keil build must report `0 Error(s), 0 Warning(s)`.

---

### Task 1: Put safety protection above communication and logging

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/task/Task_Manager/app_freertos.c`
- Test: temporary `high_risk_priority_test.ps1` (delete after verification)

**Interfaces:**
- Consumes: existing CMSIS-RTOS priority definitions.
- Produces: AFE realtime, BMS high, DataHandle high, Comm above-normal, SaveLog below-normal priority ordering.

- [ ] Write a failing static test asserting BMS is `osPriorityHigh` and remains above Comm/SaveLog.
- [ ] Run the test and verify it fails against the current `osPriorityNormal` BMS definition.
- [ ] Change only the task priority definitions.
- [ ] Run the test again and verify it passes.
- [ ] Build APP with Keil and inspect the complete log.

### Task 2: Serialize all Flash hardware and composite transactions

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/Driver/d_flash.h`
- Modify: `app/N32G45x_KEIL/User_Src/Driver/d_flash.c`
- Modify: `app/N32G45x_KEIL/Core/src/main.c`
- Modify: `app/N32G45x_KEIL/User_Src/Middle/m_flash_wl.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_current_calib.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_bms_fault.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_iap_manage.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_log_manage.c`
- Test: temporary `flash_guard_test.ps1` (delete after verification)

**Interfaces:**
- Consumes: existing Flash write/erase and wear-level APIs.
- Produces: `d_flash_init()`, `d_flash_acquire(uint32_t timeout_ms)`, and `d_flash_release()`; the guard is recursive so a composite operation can safely call the low-level driver.

- [ ] Write a failing static test requiring mutex creation, explicit initialization, recursive acquire/release, and all composite Flash writers to acquire the guard.
- [ ] Run the test and verify it fails because no guard exists.
- [ ] Implement a FreeRTOS recursive mutex in the Flash driver and fail closed if it cannot be created or acquired.
- [ ] Initialize the guard before any APP Flash writer/reader is called; reset if initialization fails.
- [ ] Wrap wear-level init/format/append/read/info and calibration, PF, IAP, and LOG Flash transactions with the guard.
- [ ] Run the test again and verify it passes.
- [ ] Build APP with Keil and inspect the complete log.

### Task 3: Make AFE and BMS shared state consistent

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.h`
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_bms_snapshot.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_bms_fault.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_log_manage.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_comm_manage.c`
- Test: temporary `shared_state_guard_test.ps1` (delete after verification)

**Interfaces:**
- Consumes: existing `gt_hsi_afe`, `app_bms_snapshot_read()`, and `gt_app_bms_fault` data.
- Produces: named AFE resource acquire/release and a protected snapshot publication path; communication/logging read the published snapshot or a protected fault copy.

- [ ] Write a failing static test requiring AFE ownership APIs and protected readers/writers.
- [ ] Run the test and verify it fails against direct unprotected shared reads.
- [ ] Add a recursive AFE mutex initialized before task creation; protect AFE I2C/control entry points and the DataHandle source-copy window.
- [ ] Publish the complete BMS snapshot while the AFE source is stable.
- [ ] Add a protected BMS fault snapshot helper and migrate communication/log collection reads that currently dereference the live fault object.
- [ ] Run the test again and verify it passes.
- [ ] Build APP with Keil and inspect the complete log.

### Task 4: Monitor high-priority task deadline misses

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/task/Task_Manager/app_freertos.h`
- Modify: `app/N32G45x_KEIL/User_Src/task/Task_Manager/app_freertos.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/AFE_Task/afe_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/BMS_Task/bms_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/DataHandle_Task/data_handle_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/Comm_Task/comm_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/StackMonitor_Task/stack_monitor_task.c`
- Test: temporary `task_deadline_monitor_test.ps1` (delete after verification)

**Interfaces:**
- Consumes: existing task heartbeat monitor and 1ms `xTaskGetTickCount()`.
- Produces: per-task last heartbeat tick, expected period, deadline miss count, and watchdog feed blocking when a safety task misses its deadline.

- [ ] Write a failing static test requiring deadline fields, per-task reporting, and watchdog blocking on safety-task deadline misses.
- [ ] Run the test and verify it fails against heartbeat-only monitoring.
- [ ] Add deadline metadata and a heartbeat API that records each task's actual period.
- [ ] Update the monitor to increment deadline misses and block feeding for AFE/BMS/DataHandle deadline failures.
- [ ] Run the test again and verify it passes.
- [ ] Build APP with Keil and inspect the complete log.

### Final verification

- [ ] Re-run all four temporary static tests.
- [ ] Confirm no temporary test files remain.
- [ ] Confirm BOOT files have not changed.
- [ ] Run the complete Keil APP build and read the full log.
- [ ] Report implemented items separately from hardware-only validation still required.
