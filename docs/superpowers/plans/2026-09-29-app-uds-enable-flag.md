# APP UDS Enable Flag Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a project parameter that defaults UDS off and, when enabled, runs a safe diagnostic-only UDS subset through the N32 CAN driver.

**Architecture:** Gate UDS initialization, standard-frame routing, and its 1 ms task with one `APP_PARAM_UDS_ENABLE` project macro. Port the UDS transmit adapter to `d_can`; expose only session control (default/extended), DID F186, and TesterPresent. Keep programming session/download and unfinished/example services unsupported.

**Tech Stack:** C, Keil MDK project XML, PowerShell contract tests, N32G45x `d_can` driver.

**Spec:** User-approved conversation scope, 2026-09-29: add a UDS enable flag in `app_param.h`; default disabled; enabling provides safe diagnostic UDS only, with programming/download kept disabled.

## Global Constraints

- Preserve unrelated existing working-tree edits.
- Do not modify the OneDrive clone; target only `E:\DUCAR\DUCAR CODE\new platform\DUCAR_N32G45x_Project`.
- `APP_PARAM_UDS_ENABLE` must be exactly `0U` or `1U`, defaulting to `0U`.
- Never expose the legacy S32K144 Flash erase/write/reset path or incomplete download handlers.

## Review Focus

- Disabled UDS must not initialize, accept standard CAN frames, tick, or transmit; test source guards at each lifecycle point.
- Enabled UDS must use standard frames via `d_can_send`; test port and project source inclusion.
- Programming session and download services must remain unsupported; test the service allowlist and absence of legacy Flash/reset hooks.
- Existing non-UDS extended-frame protocol routing must remain unchanged; test APP CAN dispatch contract.
- Invalid enable values must fail compilation; test the exact 0/1 compile-time guard.

---

### Task 1: Add failing UDS integration contract test

**Files:**
- Create: `tool/tests/uds_enable_contract_test.ps1`

- [ ] Check the default macro and binary-value validation in `app_param.h`.
- [ ] Check init/RX/1 ms task are guarded by that macro, and UDS TX uses `d_can_send` as a standard frame.
- [ ] Check only SID 0x10, 0x22, and 0x3E are registered; programming session and download handlers are not exposed.
- [ ] Check the required UDS C modules are enabled in `N32G45x.uvprojx` and unsupported SID modules remain excluded.
- [ ] Run the test and confirm it fails before implementation.

### Task 2: Wire the gated safe UDS subset and verify

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/App/app_param.h`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_can.c`
- Modify: `app/N32G45x_KEIL/User_Src/task/comm_task.c`
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSBase/uds_port.c`
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSBase/uds_service.c` (only if needed to compile/dispatch the safe subset)
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/service_cfg.c`
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/service_cfg.h`
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID10_SessionControl.c`
- Modify: `app/N32G45x_KEIL/MDK-ARM/N32G45x.uvprojx`

- [ ] Add `APP_PARAM_UDS_ENABLE` default `0U` and reject values other than 0/1.
- [ ] Guard UDS init and receive routing in `app_can.c`; leave non-UDS CAN dispatch unchanged.
- [ ] Call `uds_1ms_task()` from the existing 1 ms communication loop only when enabled.
- [ ] Replace old FlexCAN transmit with standard-ID `d_can_send(&can0, ..., D_CAN_ID_STANDARD, ...)`.
- [ ] Register only 0x10 default/extended session, 0x22/F186, and 0x3E; return unsupported for programming session and omit 0x34/0x36/0x37 and placeholder services.
- [ ] Enable only required UDS C sources in the Keil project.
- [ ] Run `uds_enable_contract_test.ps1`, existing CAN contract tests, and the Keil APP build if the compiler is available.

