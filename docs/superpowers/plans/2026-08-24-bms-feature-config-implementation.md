# BMS Feature Configuration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Centralize configurable BMS protection switches in `app_bms_config.h`, keep balance configuration in the existing `app_balance_manage.h`, and make the agreed core functions fixed-on by removing their top-level feature switches.

**Architecture:** `app_bms_config.h` owns only BMS protection enable macros and the compatibility alias for the protection master switch. `app_bms_fault_config.h` keeps protection thresholds and delays, while `app_balance_manage.h` keeps the balance switch and balance thresholds. MOS service, low power, ordinary BMS communication, IAP, current calibration, logging, MCU NTC, LED, and SOC remain unconditional application services.

**Tech Stack:** C firmware, Keil MDK-ARM project, N32G452REL7, PowerShell source checks, ARM Cortex-M4 FreeRTOS/CMSIS-RTOS application.

**Spec:** `docs/superpowers/specs/2026-08-24-bms-feature-config-design.md`

## Global Constraints

- Preserve every existing protection switch value and threshold value.
- Keep balance switch and thresholds in `app/User/App/app_balance_manage.h`.
- Do not add top-level switches for MOS service, low power, ordinary BMS communication, IAP, current calibration, logging, MCU NTC, LED, or SOC.
- Do not move RTOS, Flash-map, product-identity, or driver configuration into the BMS protection header.
- Target remains `N32G452REL7`; final verification must use the APP Keil project.

### Task 1: Define the red source-check contract

**Files:**
- Modify: `app/MDK-ARM/bms_config_source_check.ps1`
- Test: `app/MDK-ARM/bms_config_source_check.ps1`

**Interfaces:**
- Consumes: current APP configuration headers and business task sources.
- Produces: a PowerShell check that fails before the central header exists and passes only when the final configuration boundaries are present.

- [ ] **Step 1: Update the failing source check**

  Assert that `app_bms_config.h` exists and contains the protection master switch, all existing `BMS_*_ENABLE` protection switches, and the `BMS_SW_PROTECT_ENABLE` compatibility alias. Assert that the old top-level switches for fixed-on functions are absent. Assert that `app_balance_manage.h` still contains `APP_BALANCE_ENABLE` and the three existing balance thresholds. Assert that `app_bms_fault_config.h` includes the central header and no longer defines protection switches.

- [ ] **Step 2: Run the check to verify RED**

  Run:

  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File .\app\MDK-ARM\bms_config_source_check.ps1
  ```

  Expected: FAIL because `app_bms_config.h` does not yet exist.

### Task 2: Add the central protection configuration header

**Files:**
- Create: `app/User/App/app_bms_config.h`
- Modify: `app/User/App/app_bms_fault_config.h`

**Interfaces:**
- Consumes: existing protection macro names and values from `app_bms_fault_config.h`.
- Produces: one authoritative definition for the protection master switch and all protection sub-switches.

- [ ] **Step 1: Move existing protection switch definitions**

  Copy the existing `BMS_*_ENABLE` protection definitions and values verbatim into `app_bms_config.h`. Define `BMS_FEATURE_FAULT_PROTECT_ENABLE` as `1U` and map `BMS_SW_PROTECT_ENABLE` to it. Do not move thresholds or delays.

- [ ] **Step 2: Make the fault configuration consume the central header**

  Include `App/app_bms_config.h` from `app_bms_fault_config.h` and remove its duplicate protection switch definitions. Leave the threshold and delay macros in their original locations and with their original values.

- [ ] **Step 3: Run the source check to verify GREEN**

  Run the PowerShell source check from Task 1. Expected: PASS.

### Task 3: Keep balance configuration in its existing header

**Files:**
- Verify: `app/User/App/app_balance_manage.h`
- Verify: `app/User/App/app_balance_manage.c`

**Interfaces:**
- Consumes: existing `APP_BALANCE_ENABLE`, `APP_BALANCE_START_DIFF_MV`, `APP_BALANCE_STOP_DIFF_MV`, and `APP_BALANCE_MIN_CELL_MV` definitions.
- Produces: no new balance configuration file and no duplicate balance macros.

- [ ] **Step 1: Confirm the existing balance definitions remain unchanged**

  Verify the header retains values `1U`, `30U`, `10U`, and `3200U`, and that the implementation continues using those names.

- [ ] **Step 2: Run the source check**

  Run the PowerShell source check. Expected: PASS.

### Task 4: Record fixed-on feature policy and update the Keil project

**Files:**
- Modify: `app/MDK-ARM/N32G45x.uvprojx`
- Create: `docs/superpowers/verification/2026-08-24-bms-config-inventory.md`

**Interfaces:**
- Consumes: the final configuration headers and the existing APP source groups.
- Produces: a Keil-visible central header entry and an auditable inventory of configurable and fixed-on behavior.

- [ ] **Step 1: Add the new header to the existing APP configuration group**

  Add `app_bms_config.h` beside the existing APP configuration headers in the UV project without changing target, device, memory layout, or include paths.

- [ ] **Step 2: Write the configuration inventory**

  Record the current protection switch values, balance values, fixed-on feature list, and the known threshold-comment discrepancies without changing those parameters.

- [ ] **Step 3: Run source checks and inspect the UV project diff**

  Confirm the source check passes and that the UV project diff adds only the new header reference.

### Task 5: Build and verify the APP project

**Files:**
- Verify: `app/MDK-ARM/N32G45x.uvprojx`
- Verify: `app/MDK-ARM/rebuild.log` or a dated build log
- Verify: `app/MDK-ARM/Objects/N32G45x.htm`

**Interfaces:**
- Consumes: completed central configuration and Keil project update.
- Produces: compiler evidence for the N32G452REL7 APP target.

- [ ] **Step 1: Run the full source check**

  Expected: exit code 0 and `PASS: unified BMS configuration source checks`.

- [ ] **Step 2: Build the APP target with Keil**

  Use the configured Keil MDK-ARM build command for the APP target and capture a dated build log. Expected: `0 Error(s), 0 Warning(s)`.

- [ ] **Step 3: Verify the map file**

  Confirm the map contains the application entry, FreeRTOS scheduler handlers, BMS/AFE/communication/data/log task symbols, and the balance implementation.

- [ ] **Step 4: Re-run all checks and report evidence**

  Run the source check and inspect the final build/map output again. Report exact exit codes and compiler counts; do not claim completion without fresh output.
