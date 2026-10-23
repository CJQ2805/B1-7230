# CAL2 Current Calibration Implementation Plan

> **For agentic workers:** Use the current-calibration development note as the behavior specification. Implement task by task with failing tests before each behavior change.

**Goal:** Add CAN CAL2 v1 two-point current calibration while keeping one shared K/B and the legacy calibration interfaces.

**Architecture:** The CAN module owns session sequencing and replies; the calibration module owns sampling, fitting, and effective K/B. An A/B append journal in currently unused Flash log-tail pages commits complete K/B records without moving existing parameter, SOC, PF, or fingerprint pages.

**Tech Stack:** C99, FreeRTOS, N32G45X Flash, Keil, host C behavior tests.

**Spec:** `CAN两点电流校准开发说明.md` supplied by the user; physical Flash addresses are adapted to `app_flash_map.h` in this project.

## Global Constraints

- Keep UART `0x71/0x72`, CAN `E700` DLC4, and `F231` wire formats.
- CAL2 uses CAN `F233`, `E700` DLC8, and `E800` DLC8 with the exact status/echo bytes from CAL2 v1.
- Charge and discharge use the same K/B; zero calibration remains optional.
- Each point uses five distinct fresh AFE samples; raw and reference limits are ±800000 mA; spacing is at least 2000 mA.
- A single point times out after 15000 ms; an unfinished session expires 120000 ms after BEGIN; terminal result cache lasts at most 120000 ms and does not lock out a new session.
- Save the complete candidate and verify it before publishing K/B. Keep older valid K/B through every interrupted write.

## Review Focus

- A reset or power loss after the first point must leave the previous K/B effective.
- A partial Flash record must never combine old K with new B.
- Repeating a command must not resample or write Flash twice.
- The old `E700` DLC4 command must be rejected while a CAL2 session is active, then work again when it terminates.
- A terminal cached result must not prevent sleep or immediate recalibration.

---

### Task 1: Flash A/B journal

**Files:** `app/N32G45x_KEIL/User_Src/App/app_flash_map.h`, `app_current_calib.c/.h`, host tests.

- [ ] Write host tests for UC fallback, committed record selection, interrupted append, page rotation, and unchanged current on save failure; observe failures.
- [ ] Reserve the free pages `0x0807F000` and `0x0807F800`; implement a 36-byte CRC/commit-last record and generation selection.
- [ ] Run the journal behavior tests and existing calibration tests.

### Task 2: Two-point measurement and fitting

**Files:** `app_current_calib.c/.h`, new CAL2 sampling module if needed, host tests.

- [ ] Write failing behavior tests for five fresh samples, stability/freshness/saturation checks, signed point pairs, limits, and third-point output.
- [ ] Implement candidate fitting, quantized residual checks, deferred save, and shared K/B publication.
- [ ] Run calibration behavior tests.

### Task 3: CAN CAL2 session and response

**Files:** `ducar_can_protocol.c/.h`, `app_can.c`, new focused CAL2 session module, host tests, Keil project.

- [ ] Write failing CAN tests for `F233`, DLC4/DLC8 routing, all five operations, echo bytes, sequence/retry rules, asynchronous completion, and 120-second expiry.
- [ ] Implement the session state machine and bounded pending `E800` response queue.
- [ ] Run CAN protocol and CAL2 behavior tests.

### Task 4: Operation and sleep interlocks

**Files:** CAN/UART/IAP/UDS ingress, `app_low_power_manage.c`, host tests.

- [ ] Write failing tests for conflicts during active CAL2 and sleep release after terminal response handling.
- [ ] Gate other writes, log reads, upgrade ingress, and legacy calibration while active; keep awake only for active work and pending response.
- [ ] Run focused tests and a Keil rebuild; inspect the build log and verify the actual memory map.
