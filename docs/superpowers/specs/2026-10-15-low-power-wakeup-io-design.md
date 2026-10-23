# Low-Power Peripheral Shutdown and Wakeup Design

## Goal

Reduce board current during STOP0 by explicitly disabling the requested indicator, CAN isolated-side supply, CAN peripheral, MCU NTC supply, and ADC, while preserving reliable wakeup and restoring normal operation after wake.

## Confirmed requirements

- Explicitly turn off the five battery LEDs on PD2–PD6 and the PC13 indicator before STOP0.
- Drive ISO5V_EN low during STOP0.
- Use CAN_WK as a low-active falling-edge wake source only while entering/remaining in STOP0. After any wake, close its EXTI and restore it to a normal input so a held-low signal cannot keep generating interrupts.
- Disable CAN and configure CAN_TX/RX (PA12/PA11) for low-power analog GPIO while asleep. Restore CAN pins and controller after wake.
- Configure MCU_SW (PD1) press/falling edge as a wake source.
- Configure DETD (PC6) and DETC (PD15) falling edges as wake sources, per the user's correction.
- Turn off MCU_NTC_EN and disable ADC1/ADC2 before STOP0; restore ADC and NTC sampling after wake.
- Preserve existing RTC 20-second watchdog servicing, AFE_ALERT wake, UART RX wake, current/idle policy, and post-wake scheduling behavior unless needed for safe sequencing.

## Entry sequence

1. Keep the existing 30-second idle qualification and 200 mA current limit. Before power changes, require no active IAP/calibration or CAN protocol work. Enter a CAN-quiescing state that prevents the communication task from enqueueing new frames, then require all CAN transmit mailboxes empty before disabling the controller.
2. Wait for UART TX completion and acquire the AFE lock as today.
3. Suspend ADC conversions through a reversible ADC API. The API must prevent new reads, wait for active reads to finish, disable ADC1/ADC2 and TEMP/VREFINT, and gate their clocks. If suspend cannot complete, abort STOP entry and restore normal operation.
4. Deinitialize CAN so interrupts and the controller are disabled and PA11/PA12 become analog. Configure AFE_ALERT, UART RX, CAN_WK, MCU_SW, DETD, and DETC wake EXTI lines with the confirmed polarities. CAN_WK, MCU_SW, DETD, and DETC use no internal pull because the board provides the relevant external bias networks; UART RX retains its existing pull-up configuration.
5. Drive ISO5V_EN low only after CAN is disabled and CAN_WK EXTI is armed. Keep the existing RTC/watchdog preparation and switch clock.
6. Suspend the FreeRTOS scheduler, then apply final GPIO power states: set PD2–PD6 and PC13 to their off levels and drive MCU_NTC_EN low. Doing this after scheduler suspension prevents the LED task from toggling PC13 or the NTC task from re-enabling its supply during the final entry window. Enter STOP0 immediately afterward.

## Wake and restore sequence

1. The ISR callbacks only record a wake-source flag; no CAN, ADC, or AFE transaction is performed in interrupt context.
2. Immediately after STOP returns, close all temporary wake EXTI lines, including CAN_WK, before restoring pins. This ensures CAN_WK returns to a normal input and cannot repeatedly interrupt while held active.
3. Restore the system clock and board GPIO defaults, which re-enable ISO5V_EN and restore normal CAN_WK/MCU_SW/DETD/DETC input modes.
4. Reinitialize CAN directly through the driver (not `app_can_init`, which resets protocol state), resume ADC, restore NTC supply, UART, timers, and AFE, and re-evaluate MOS outputs through existing fault gating. Only then clear CAN-quiescing state.
5. Resume the scheduler only after all wake-time peripherals are ready. External-edge wakes follow the existing non-RTC idle qualification; RTC-only wakes retain the existing quick re-sleep policy.

## Failure handling and safety

- Do not enter STOP if CAN transmit is not idle, protocol work is active, ADC cannot quiesce, or an existing clock/RTC/watchdog preparation step fails. Bound waits; never hold the scheduler suspended while waiting for an active ADC conversion or CAN mailbox.
- Any failed entry step must restore ISO5V_EN, CAN, ADC, NTC, GPIO, and AFE state that was already changed before returning to normal scheduling.
- Do not disable DCDC_EN, MOS_DR_EN, or V5V_EN in this change; their board-level dependencies are not established by the supplied requirement.
- This change pauses MCU software tasks during STOP. The DVC1124 Sleep-mode behavior for autonomous protection, ALERT, and MOS control must be validated against its datasheet and hardware before relying on it as the sole protection path during STOP.

## Verification

- Compile the exact E-drive Keil APP project.
- Inspect the built flow to confirm every wake EXTI is closed and every peripheral is restored before scheduler resume.
- Bench-check each falling-edge source independently (CAN_WK, MCU_SW, DETD, DETC), plus existing AFE_ALERT, UART RX, and RTC wake.
- With a held-low CAN_WK, verify the MCU wakes once, CAN_WK becomes a normal input after wake, and no repeated interrupt loop occurs.
- Measure STOP current with CAN ISO5V_EN low, ADC/NTC off, and all indicators off; compare with the same board before the change.
- Verify ADC-based temperature sampling, CAN traffic, and LED behavior recover after each wake source.
