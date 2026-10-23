# BMS Operating Status Design

## Public result

`app_bms_status.c/.h` owns one externally visible `uint8_t` result, `gu8AppBmsStatus`. No public state structure or init function is needed. Status values are 0=static, 1=discharge, 2=charge, and 3=feedback/regenerative charge.

## Inputs and priority

The module consumes the already-processed `current_10ma` value and debounced DETC/DETD presence from the BMS snapshot. Positive current is the charge direction, negative current is discharge, and zero means no current. Do not add another current threshold.

Priority is DETC, then current direction, then DETD: DETC present always selects charge immediately. Without DETC, positive current selects feedback until stable; negative current selects discharge; with zero current, DETD present selects discharge and DETD absent selects static.

## Charge stability

With DETC absent and positive processed current, require 5 seconds of positive current whose rolling 5-second peak-to-peak range is at most 500mA. `current_10ma` is in 10mA units, so the limit is 50 counts. Use the kernel's monotonic tick and its reported frequency for elapsed time; do not infer time from the number of DataHandle_Task iterations. A sample gap over 100ms invalidates continuity and restarts qualification from the current sample. A rolling 5-second range over 50 counts immediately returns the state to feedback and restarts qualification from the current sample. A zero or negative current immediately cancels qualification and selects static/discharge by the rules above.

## Integration and verification

Call the update routine once per refreshed snapshot from `DataHandle_Task`. Add the source file to the Keil APP project. Host tests cover all status priorities, the 500mA inclusive boundary, 5-second qualification, fluctuation fallback/requalification, and zero/negative-current timer cancellation.
