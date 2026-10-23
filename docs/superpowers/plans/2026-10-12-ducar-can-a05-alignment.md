# DUCAR CAN A0.5 Alignment Implementation Plan

> **For agentic workers:** Implement task-by-task using the existing host behavior harness and target Keil build. Preserve all unrelated working-tree changes.

**Goal:** Align the embedded DUCAR CAN behavior with the A0.5 workbook changes.

**Architecture:** Keep protocol serialization and command handling in `ducar_can_protocol.c`; keep product/protocol version configuration in `app_param.h`. Extend the existing host behavior test so wire bytes and SOC application are verified independently of the hardware.

**Tech Stack:** C11, MSVC host test harness, Keil MDK/ArmClang target project.

**Spec:** `E:/DUCAR/公司文件/管控类型-通信协议/CAN/DUCAR_CAN通信协议A0.5.xlsx`.

## Global Constraints

- Preserve existing user changes in the working tree; the actual project is already dirty.
- F216 pre-MOS has no source yet and must be transmitted as S8 invalid `-128` (`0x80`); do not alias `NTC_PTC`.
- E200 SOC is a single unsigned integer percent in Data0; 75% is `0x4B` (the workbook's `0x48=75` text is erroneous).
- Keep the existing asynchronous SOC validation/persistence and E8 ACK/NAK behavior.
- F221's balance bit 0 is logical cell 1 and is serialized little-endian.

## Review Focus

- Invalid or absent MOS NTC values must not become plausible temperatures.
- F21F charge/discharge lock bits must reflect software lock state, not instantaneous AFE faults.
- E200 must not regress to a 16-bit SOC parse or process an empty payload.
- F221 must expose the successfully applied logical balance mask in little-endian order.
- F200 minor version must fit its revised one-byte field without overwriting Data2.

---

### Task 1: Implement A0.5 CAN field and control alignment

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_param.h`
- Modify: `tool/tests/can_a03_behavior_test.c`
- Modify: `tool/tests/run_can_a03_behavior_test.ps1` only if output labeling needs updating

**Interfaces:**
- Consume: `app_balance_get_mask(void) -> uint32_t`; `app_bms_soc_write_request(uint8_t)`.
- Produce: F216 S8 temperatures, F221 U32 mask, F21F lock bits, F200 A0.5 version bytes, and E200 Data0 SOC semantics.

- [ ] Add behavior assertions for F216, F221, F21F, F200, E200, and E8; run the host test and observe expected failures.
- [ ] Implement the minimum serialization/receive changes and set package T version/protocol version to T10/A0.5.
- [ ] Re-run the host behavior test and the relevant CAN contract tests.
- [ ] Build the actual `N32G45x` Keil target and inspect the complete build result.
- [ ] Run `git diff --check` and inspect only the scoped changes; do not commit because the shared main working tree contains pre-existing user changes in overlapping files.
