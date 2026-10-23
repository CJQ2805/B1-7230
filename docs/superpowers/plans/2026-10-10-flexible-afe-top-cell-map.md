# Flexible AFE Top-Cell Mapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the logical pack series count configurable (20S/21S) while reading all 24 DVC1124 physical channels and mapping the physical top-cell channel into the logical last-cell slot.

**Architecture:** Separate logical series count, raw AFE channel capacity, and scan count. Normalize cell voltages in the BMS snapshot and translate logical balance bits back to AFE physical bits. The configured top-cell physical channel defaults to index 23.

**Tech Stack:** C for the firmware and C/PowerShell contract tests used by this repository.

**Spec:** Approved design in the current conversation: for logical count N, logical cells 0..N-2 map to the same physical indices, and logical N-1 maps to physical channel 23; AFE reads all 24 channels.

## Global Constraints

- Keep `APP_BMS_MAX_CELL_NUM` as the 24-slot capacity.
- Keep the active logical series count configurable; current value is 20.
- Always scan 24 AFE channels for this wiring.
- Preserve logical indexing for calibration, protection, and CAN/UART consumers.
- Map the logical last-cell balance bit back to physical AFE bit 23.

## Review Focus

- 20S must map logical slot 19 to physical channel 23 without exposing physical zero channels 19..22.
- 21S must map logical slot 20 to physical channel 23 and retain physical channel 19 as logical slot 19.
- 24S must remain an identity mapping with no duplicate or omitted logical cells.
- AFE scan completion must still wait for all 24 physical channels, while snapshot readiness checks only the configured logical cells.
- Balance translation must map the logical top-cell bit to physical bit 23 and preserve all other active bits.

---

### Task 1: Define and test topology mapping

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/App/app_param.h`
- Create: `app/N32G45x_KEIL/User_Src/App/app_cell_topology.h`
- Create: `tool/tests/app_cell_topology_test.c`
- Test: `tool/tests/app_cell_topology_test.c`

**Interfaces:**
- Produces `app_bms_topology_logical_to_afe_index(logical_index, logical_count, top_afe_index)` and `app_bms_topology_balance_to_afe(logical_mask, logical_count, top_afe_index)`.

- [ ] Write failing C unit tests for literal 20S, 21S, and 24S mappings and balance-mask translation.
- [ ] Run the test and verify failure before the helper exists.
- [ ] Add the configured logical count (20), scan count (24), top physical index (23), and header-only mapping helpers.
- [ ] Run the unit test and verify all mapping assertions pass.

### Task 2: Integrate normalized reads and balance output

**Files:**
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.h`
- Modify: `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_bms_snapshot.c`
- Modify: `app/N32G45x_KEIL/User_Src/App/app_balance_manage.c`
- Test: existing AFE/snapshot/balance contract tests, plus Task 1 unit test

**Interfaces:**
- Consumes Task 1 topology helpers.
- AFE retains a 24-channel raw buffer and scans 24 channels; the snapshot emits the configured logical count.

- [ ] Integrate the failing-tested map into snapshot reads and balance-mask conversion.
- [ ] Run topology, AFE, and balance-related tests.
- [ ] Review the full diff and run the available Keil/project verification; report if the Keil toolchain is unavailable.
