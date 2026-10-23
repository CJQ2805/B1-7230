# Low-Power Peripheral Shutdown and Wakeup Implementation Plan

## Goal

Implement the approved low-power design in the E-drive firmware so STOP0 explicitly shuts down the requested loads and peripherals, wakes on CAN_WK / MCU_SW / DETD / DETC falling edges, and restores normal operation before FreeRTOS tasks resume.

## Architecture

- `App/app_low_power_manage.c` remains the entry/wake orchestrator and owns rollback ordering.
- `Driver/d_adc.c` gains a reversible suspend/resume path distinct from the permanent power-down latch used by shutdown.
- The CAN driver provides a global TX inhibit so every direct protocol caller is covered; the APP CAN layer owns protocol eligibility, bounded mailbox drain, and sleep/wake coordination. Driver initialization preserves an existing inhibit; boot initialization explicitly enables TX, while STOP wake enables it only after peripherals are restored.
- Board pin definitions remain the source of truth. Temporary EXTI setup is removed on every wake/abort path; `board_io_init()` restores normal pin modes.
- The scheduler is suspended only for the final GPIO changes and STOP transition. No waits or resource acquisition occur while it is suspended.

## Tech Stack and Verification

- C11 firmware for N32G45x, FreeRTOS, vendor SPL, Keil MDK project `app/N32G45x_KEIL/MDK-ARM/N32G45x.uvprojx`.
- Add a PowerShell source-contract test under `tool/tests/`, following the existing test convention; run it red before implementation and green afterward.
- Run the relevant existing CAN / low-power-adjacent contract and behavior tests, then compile the exact E-drive Keil APP target. Do not build or edit the OneDrive copy.
- Hardware-only checks (wake edges, held-low CAN_WK, STOP current, and resumed ADC/CAN/LED behavior) are documented as bench validation and are not claimed as completed by a host build.

## Specification

`docs/superpowers/specs/2026-10-15-low-power-wakeup-io-design.md`

## Global Constraints

- Preserve the existing 30-second normal idle qualification, current threshold, 20-second RTC/IWDG service cadence, and RTC-only quick re-sleep behavior; do not revisit the previously reported immediate-wake timing issue.
- Do not re-enable the intentionally commented `zs_b1_can_protocol_task()`.
- Do not change `DCDC_EN`, `MOS_DR_EN`, or `V5V_EN` behavior.
- Do not use `app_can_init()` on wake because it resets protocol state; restore the CAN driver directly.
- Preserve existing AFE_ALERT and UART RX wake behavior, MOS fault gating, and business tick handling.
- User confirmed falling-edge wake for DETD/DETC. CAN_WK, MCU_SW, DETD, and DETC temporary EXTI must be closed immediately after STOP returns so held-low inputs cannot cause an interrupt loop.
- Every partial-entry failure must restore already changed modules and release the AFE resource; never block while the scheduler is suspended.

## Review Focus

- Verify that ADC suspension rejects new reads before waiting for in-flight samples, is bounded, and is reversible without weakening permanent shutdown behavior.
- Verify that the CAN TX inhibit covers all `d_can_send()` callers (including receive-triggered UDS/IAP responses), that active protocol sessions prevent sleep, and that mailbox drain is bounded.
- Verify exact wake polarity/pull settings and pin mapping against `board_io.h`; close wake EXTI before restoring the default GPIO/CAN modes.
- Verify final LED and MCU_NTC_EN off writes occur after scheduler suspension, while CAN_WK EXTI is armed before ISO5V_EN is disabled.
- Verify all failure paths unwind CAN inhibit/deinit, ADC suspension, ISO5V_EN, GPIO, AFE, and wake EXTI state in a safe order.
- Verify restoration completes before scheduler resume and does not reset protocol state or alter RTC policy.

## Implementation Tasks

### Task 1: Add a failing host behavior test for the low-power sequence

Files:

- Create `tool/tests/app_low_power_peripheral_behavior_test.c` and `tool/tests/run_app_low_power_peripheral_behavior_test.ps1`.
- Add narrowly scoped fake RTOS / board / peripheral implementations under `tool/tests/host_stubs/low_power/` so the test links and executes the real `App/app_low_power_manage.c`.

Behavior cases:

- Drive the real public idle task through its existing 30-second qualification and assert the fake hardware event trace shows prepare/quiesce, wake-source setup, requested power-off, STOP, wake cleanup, restoration, and scheduler resume in order.
- Inject RTC setup failure and CAN-quiesce failure; assert STOP is not entered and resources already changed are restored/released.
- Trigger each newly added wake callback through the real EXTI registration and verify its wake reason is recorded and every temporary EXTI is closed before normal GPIO restoration.
- Assert the existing AFE_ALERT, UART RX, RTC 20-second, and RTC quick re-sleep behavior remains intact.

Expected RED: the new behavior test builds against the current interfaces but fails on missing expected hardware events / wake callbacks. If it fails to compile, fix only the test harness until it reaches the expected behavioral failure.

### Task 2: Add ADC reversible-suspend behavior tests, then implement the ADC API

Files:

- Create a focused host test/runner that compiles the real `Driver/d_adc.c` with fake vendor ADC/RCC and FreeRTOS primitives.
- Modify `app/N32G45x_KEIL/User_Src/Driver/d_adc.h` and `.c`.

Tests first:

- Confirm suspend disables ADC1/ADC2, TEMP/VREFINT, and clocks without setting the permanent shutdown latch.
- Confirm reads are rejected while suspended and succeed after resume/recalibration.
- Confirm a failed/timeout suspend leaves normal sampling available, while permanent `d_adc_deinit()` still prevents later initialization.

Implementation:

- Set a reversible suspend gate atomically before waiting, preventing new `d_adc_read()` operations from entering.
- Wait for active reads to exit with a bounded timeout. On timeout, clear the temporary gate and leave ADC hardware operational; report failure.
- Reinitialize and calibrate while reads remain gated on resume; allow reads only after readiness. Keep permanent `d_adc_deinit()` semantics unchanged.

Expected sequence: observe each new test fail against current `d_adc.c`, implement the smallest behavior, then run the ADC host test and its regression suite green.

### Task 3: Add CAN sleep-coordination behavior tests, then implement TX quiescing

Files:

- `app/N32G45x_KEIL/User_Src/Driver/d_can.h` / `.c`
- `app/N32G45x_KEIL/User_Src/App/app_can.h` / `.c`
- Add focused host tests/runner for the real APP CAN coordination and driver TX gate, using vendor/protocol fakes only at the external boundaries.
- Relevant protocol headers/implementations only where an existing active-session query must be exposed.

Tests first:

- A call to real `d_can_send()` is rejected while TX is inhibited and resumes after explicit enable.
- Real `app_can_prepare_sleep()` refuses active protocol work, inhibits new direct sends, waits for all three hardware mailboxes, and restores sending on failure.
- A successful wake-side driver init keeps TX inhibited until explicit APP resume; protocol initialization/state is not reset.

Implementation:

- Add an atomic driver TX gate checked by `d_can_send()` to cover DUCAR, IAP, UDS, and receive-triggered response callers.
- Have APP CAN reject active protocol work, inhibit TX, then wait a bounded time for all three hardware mailboxes to become idle; cancel the inhibit on failure.
- Extend sleep eligibility for active DUCAR transmit/transport work, IAP, CAL2, and any active UDS session capable of traffic, using actual status APIs found in the implementation.
- Preserve the TX inhibit across `d_can_deinit()` / `d_can_init()`. Boot initialization enables TX explicitly; STOP wake uses direct driver init and enables TX only after the wake sequence is ready. Keep `zs_b1_can_protocol_task()` commented out and avoid resetting protocol state.

Expected sequence: run the driver and APP CAN tests red before implementation, then green plus existing CAN contract/behavior tests.

### Task 4: Extend the host low-power behavior test for wake GPIO and explicit off states

Files:

- `app/N32G45x_KEIL/User_Src/App/app_low_power_manage.c`
- `app/N32G45x_KEIL/User_Src/Board/board_io.h` / `.c` only if a missing pin helper/definition is needed.
- Extend `tool/tests/app_low_power_peripheral_behavior_test.c` with scenarios before changing production code.

Test first: assert falling-edge EXTI registration for CAN_WK, MCU_SW, DETD, and DETC (no internal pull), retention of AFE_ALERT/UART RX, and correct callback reason; assert all six temporary EXTI sources close immediately after STOP returns. Assert PD2–PD6 and PC13 turn off after scheduler suspension, ISO5V_EN goes low only after CAN_WK EXTI is armed, and MCU_NTC_EN goes low after ADC suspension/scheduler suspension.

Implementation:

- Add callback flags and wake reasons for the four new inputs; callbacks only set flags.
- Configure/close all temporary EXTI on success and all abort paths.
- Use existing LED setters for the five battery LEDs, explicitly turn off PC13, disable ISO5V_EN after CAN is disabled and CAN_WK EXTI is armed, and turn MCU_NTC_EN off only at the final scheduler-suspended boundary.
- Preserve non-requested power outputs and PA3 analog configuration.

Expected sequence: test fails on missing/wrong wake and power events, then passes with the production low-power module.

### Task 5: Integrate transactional entry, wake, and rollback; finish the integration test

Files:

- `app/N32G45x_KEIL/User_Src/App/app_low_power_manage.c`
- Update the host test only for any newly discovered observable behavior, writing/observing each added assertion before its corresponding implementation change.

Entry order:

1. Keep current idle qualification; prepare CAN sleep (eligibility, TX inhibit, bounded drain), and wait for UART TX completion before taking the AFE resource.
2. Acquire the AFE resource as today, then suspend ADC.
3. Enter AFE low power, deinitialize CAN, configure wake EXTI, start RTC/watchdog low-power support, and switch clock using existing checks.
4. After CAN_WK EXTI is armed, disable ISO5V_EN.
5. Suspend the scheduler; apply final battery LED / PC13 off states and MCU_NTC_EN low; feed watchdog; then perform the existing protected STOP0 transition.
6. If any pre-STOP step fails, unwind safely: restore clock if needed, close EXTI, restore CAN power and driver, resume ADC and NTC, restore GPIO/AFE, cancel CAN quiescing, release AFE, and reset idle qualification.

Wake order:

1. Exit watchdog low-power handling and immediately close all temporary EXTI lines.
2. Restore clock and `board_io_init()` defaults, including ISO5V_EN and normal CAN_WK input mode.
3. Restore CAN directly while TX remains inhibited; resume ADC, MCU NTC supply, timers/UART/AFE, and fault-gated MOS outputs.
4. Clear CAN quiescing / enable TX only after all required interfaces are ready; resume the scheduler last.
5. Preserve existing RTC-vs-external wake classification and 20-second / 1-second sleep policy.

Expected sequence: the integration behavior test passes for normal entry/wake and each injected failure, with all restoration events preceding scheduler resume.

### Task 6: Run regression checks and prepare bench validation

Run:

- `tool/tests/run_app_low_power_peripheral_behavior_test.ps1`
- ADC and CAN host behavior test runners added above.
- Existing adjacent tests: `app_input_signals_result_contract_test.ps1`, `hsi_afe_dvc1124_contract_test.ps1`, CAN contract/behavior tests, and `app_h_power_detd_precharge_behavior_test.ps1`.
- Build the exact E-drive `N32G45x.uvprojx` APP target; do not invoke packaging or alter generated release artifacts unless the normal build requires them.

Record hardware checks for the user to perform: individual falling-edge wake on CAN_WK / MCU_SW / DETD / DETC, held-low CAN_WK wakes once only, existing AFE_ALERT/UART/RTC wake, current consumption, and ADC/CAN/LED restoration after each wake.

## Completion Criteria

- New host behavior tests fail before implementation and pass afterward.
- Exact E-drive Keil APP target builds successfully, or the specific environmental blocker is reported with evidence.
- No temporary wake EXTI remains active after wake/abort; all requested loads/peripherals are restored before task scheduling resumes.
- The user receives a concise summary of changed files, verification results, and bench checks still needed.
