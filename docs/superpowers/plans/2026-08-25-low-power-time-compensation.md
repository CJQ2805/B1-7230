# APP Low-Power Time Compensation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make APP STOP0 sleep and wake-up time handling consistent: RTC calendar time keeps running automatically, FreeRTOS catches up the elapsed sleep ticks, TIM6/TIM7 1ms counters remain monotonic, and periodic `osDelayUntil()` tasks resume from a fresh period boundary without backlog spinning.

**Architecture:** Before STOP0, the APP task suspends the FreeRTOS scheduler only across the STOP entry and peripheral restoration window. The RTC calendar is sampled before and after STOP; the elapsed seconds are converted to milliseconds, with the configured RTC wake interval as a fallback when the calendar has not been set. After TIM6/TIM7 are reinitialized, their pre-sleep counters are restored plus the elapsed time. The scheduler is resumed, missed ticks are caught up with `xTaskCatchUpTicks()`, and a global wake generation causes each periodic task to reset its local `osDelayUntil()` baseline once.

**Tech Stack:** N32G45x APP, Keil MDK/ARMClang, CMSIS-RTOS v1 over FreeRTOS, RTC calendar, TIM6/TIM7 1ms interrupts, PowerShell source checks.

**Spec:** User-approved design in this conversation: RTC calendar requires no software correction; task and TIM7 time bases are compensated after STOP; RTC wake period is 120 seconds; only APP changes are allowed.

## Global Constraints

- Only APP files under `app\N32G45x_KEIL`, APP verification scripts, and this plan may change.
- The `boot` directory and BOOT Keil project must remain unchanged.
- Keep the current `osDelayUntil()` task architecture; do not reintroduce lock/unlock resource wrappers.
- RTC calendar is never advanced by writing a calculated value back to the RTC.
- Hardware verification on a physical board is not available in this workspace; source checks and Keil compilation are required, while STOP wake timing remains a board-level verification item.

---

### Task 1: Add failing source checks for sleep-time compensation

**Files:**
- Modify: `app\N32G45x_KEIL\MDK-ARM\freertos_business_tasks_source_check.ps1`

- [x] Assert the RTC wake interval is exactly `120U`.
- [x] Assert the low-power restore path uses `d_timer_reinit_after_stop()` and does not reset the counters with plain `d_timer_init()`.
- [x] Assert the STOP path suspends/resumes the scheduler, calls `xTaskCatchUpTicks()`, and notifies the task wake-generation synchronizer.
- [x] Assert the task manager exposes the wake-generation synchronizer and every periodic task invokes it before its business loop delay.
- [x] Run the source check and confirm it fails before production changes.

### Task 2: Preserve and compensate TIM6/TIM7 counters across STOP

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\Driver\d_timer.h`
- Modify: `app\N32G45x_KEIL\User_Src\Driver\d_timer.c`

- [x] Add `d_timer_reinit_after_stop(uint32_t elapsed_ms)`.
- [x] Preserve both counters before the peripheral reinitialization, reconfigure TIM6/TIM7, then restore each counter plus elapsed sleep time.
- [x] Keep startup `d_timer_init()` behavior unchanged.

### Task 3: Add task-period resynchronization after wake

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\task\Task_Manager\app_freertos.h`
- Modify: `app\N32G45x_KEIL\User_Src\task\Task_Manager\app_freertos.c`
- Modify: all six APP task source files under `User_Src\task`

- [x] Add a global wake generation increment API.
- [x] Add a helper that resets a task's local `osDelayUntil()` baseline once per wake generation.
- [x] Invoke the helper at the top of each task loop, retaining the existing 5ms/10ms periods and priorities.

### Task 4: Measure RTC elapsed time and compensate the scheduler in low-power recovery

**Files:**
- Modify: `app\N32G45x_KEIL\User_Src\App\app_low_power_manage.c`

- [x] Change the actual RTC wake interval from 20 seconds to 120 seconds.
- [x] Capture RTC calendar time before STOP and after wake, converting valid calendar differences to milliseconds.
- [x] Use the configured RTC wake interval only as a fallback for RTC wake when the calendar is not valid; use zero for an unknown short external wake.
- [x] Keep the scheduler suspended across STOP and hardware restoration, then restore it before calling `xTaskCatchUpTicks()` as required by the FreeRTOS implementation.
- [x] Restore TIM6/TIM7 with the measured elapsed time, notify task-period resynchronization, and catch up FreeRTOS ticks.
- [x] Preserve existing wake-source classification and RTC re-sleep behavior.

### Task 5: Verify APP build and boundaries

**Files:**
- Verify: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx`
- Verify: `app\N32G45x_KEIL\MDK-ARM\low_power_time_compensation_build.log`
- Verify: `boot\**`

- [x] Run the source-level regression checks and require PASS.
- [x] Build the APP target with Keil and require 0 errors and 0 warnings.
- [x] Re-run the task layout/source checks.
- [x] Confirm the BOOT directory file count remains 173 and no BOOT path was edited by this change.
- [x] Report that physical RTC/STOP wake timing still requires board validation.
