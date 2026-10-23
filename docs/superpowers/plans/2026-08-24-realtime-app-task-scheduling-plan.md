# APP Real-Time Task Scheduling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rework the APP FreeRTOS task schedule into deterministic real-time loops without changing BOOT.

**Architecture:** AFE and DataHandle run at 10ms, with DataHandle owning MCU NTC refresh and the BMS snapshot update. BMS consumes a coherent snapshot copy at 10ms; communication runs at 5ms, while log and low-power logic become event/elapsed-time driven instead of inheriting 1-second assumptions.

**Tech Stack:** N32G45x APP, Keil MDK/ARMClang, CMSIS-RTOS v1 over FreeRTOS, PowerShell source checks.

**Spec:** `docs/superpowers/specs/2026-08-24-realtime-app-task-scheduling-design.md`

## Global Constraints

- Only APP files under `app\N32G45x_KEIL` and APP verification scripts may change.
- The `boot` directory and BOOT Keil project must remain byte-for-byte unchanged.
- Do not add FreeRTOS resource Mutex/lock/unlock APIs.
- Every periodic task loop must use `osDelayUntil()` with its declared period.

---

### Task 1: Add failing real-time source checks

**Files:**
- Modify: `app\N32G45x_KEIL\MDK-ARM\freertos_business_tasks_source_check.ps1`
- Modify: `app\N32G45x_KEIL\MDK-ARM\task_layout_source_check.ps1`

**Interfaces:**
- Consumes the current task source and project layout.
- Produces red checks for missing DataHandle snapshot ownership, old `osDelay`, old low-power signature, or old 1-second task counters.

- [ ] Add assertions that `app_bms_snapshot_update()` exists only in `DataHandle_Task` and that BMS/fault code uses `app_bms_snapshot_read()`.
- [ ] Add assertions for `osDelayUntil()` in all six task sources and reject periodic `osDelay()` calls.
- [ ] Add assertions for 10ms DataHandle/AFE/BMS/SaveLog/Default loops and 5ms Comm loop.
- [ ] Add assertions that `app_low_power_task()` receives a millisecond argument and that the old 1-second call assumptions are absent.
- [ ] Run the checks and confirm they fail against the current timing implementation.

### Task 2: Make snapshot ownership real-time and coherent

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.h`
- Modify: all APP consumers currently calling `app_bms_snapshot_get()`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_fault.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\DataHandle_Task\data_handle_task.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\BMS_Task\bms_task.c`

**Interfaces:**
- Produces `void app_bms_snapshot_read(app_bms_snapshot_t *snapshot)`.
- `DataHandle_Task` is the only task-level caller of `app_bms_snapshot_update()`.

- [ ] Add a short-critical-section snapshot copy API and update documentation.
- [ ] Convert all consumers from pointer-return `app_bms_snapshot_get()` to local coherent copies.
- [ ] Move snapshot update and NTC sampling into the 10ms DataHandle loop.
- [ ] Remove the fault-layer emergency snapshot update so the single writer remains DataHandle.
- [ ] Run source checks and confirm snapshot ownership assertions pass.

### Task 3: Convert task loops and priorities to deterministic timing

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\task\AFE_Task\afe_task.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\Comm_Task\comm_task.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\SaveLog_Task\save_log_task.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\Default_Task\default_task.c`
- Modify: `app\N32G45x_KEIL\User_Src\task\Task_Manager\app_freertos.c`

**Interfaces:**
- Task functions keep their existing CMSIS-RTOS entry signatures.
- Periods become 10ms for AFE/SaveLog/Default/DataHandle/BMS and 5ms for Comm.

- [ ] Replace each task’s `osDelay()` with `osDelayUntil()` and initialize a per-task wake timestamp.
- [ ] Remove old slow-period counters from AFE and DataHandle.
- [ ] Set priorities to Realtime/High/AboveNormal/Normal/BelowNormal/Low according to the design.
- [ ] Run the task source checks before compiling.

### Task 4: Make low power and balance logic compatible with 10ms scheduling

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\App\app_low_power_manage.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_low_power_manage.h`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_balance_manage.c`
- Modify: `app\N32G45x_KEIL\User_Src\HSI_afe\hsi_afe.c`

**Interfaces:**
- Produces `void app_low_power_task(uint32_t elapsed_ms)`.
- `hsi_afe_balance()` remains boolean and only writes when the requested mask changes.

- [ ] Replace second-count accumulation with elapsed-millisecond accumulation while preserving 30s normal and 1s RTC-resleep thresholds.
- [ ] Update comments and header contract to describe 10ms callers.
- [ ] Keep balance evaluation at 10ms but suppress repeated IIC writes for an unchanged mask.
- [ ] Run source checks and compile the APP.

### Task 5: Final verification and BOOT boundary check

**Files:**
- Verify: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx`
- Verify: `app\N32G45x_KEIL\MDK-ARM\realtime_task_build.log`
- Verify: `boot\**`

- [ ] Build the APP target with Keil and require `0 Error(s), 0 Warning(s)`.
- [ ] Confirm the APP binary/hex/factory package are produced.
- [ ] Confirm no APP-specific lock/unlock or old timing references remain.
- [ ] Recompute BOOT file count and SHA-256 fingerprint and require the pre-change values: 173 files and `5019D6D9BFB84C5F4E3A8BE320711BBE48A1C08AB6756D4BFBB1BF765BFAF578`.
