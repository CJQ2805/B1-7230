$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'app\N32G45x_KEIL\User_Src'
function Require-Pattern([string]$file, [string]$pattern, [string]$description) {
    $content = Get-Content -LiteralPath (Join-Path $src $file) -Raw -Encoding UTF8
    if ($content -notmatch $pattern) { throw "Missing: $description" }
}
Require-Pattern 'App\app_bms_snapshot.c' 'current_ma\s*=\s*gt_hsi_afe\.curr_calibrated' 'calibrated current retained for SOC'
Require-Pattern 'App\app_param.h' '#define\s+APP_PARAM_CURRENT_ZERO_DEADBAND_MA\s+200\b' 'configured 200 mA deadband'
Require-Pattern 'App\app_bms_snapshot.c' 'current_business_ma\s*=\s*gt_hsi_afe\.curr_real' 'snapshot receives post-deadband current'
Require-Pattern 'Protocol\Comm\ducar_can_protocol.c' 's32Current01A\s*=\s*tBmsSnapshot\.current_business_ma\s*/\s*10' 'DUCAR CAN current in 10 mA units'
Require-Pattern 'Protocol\Comm\ducar_uart_protocol.c' 'ducar_bms_put_i32\(au8ResponseData,\s*&u8ResponseLength,\s*ptSnapshot->current_business_ma\)' 'DUCAR UART current in mA'
Require-Pattern 'Protocol\Comm\zs_b1_can_protocol.c' 'discharge_ma\s*=\s*\(snapshot->current_business_ma\s*<\s*0\)\s*\?\s*zs_b1_abs_current_ma\(snapshot->current_business_ma\)' 'ZS discharge current'
Require-Pattern 'Protocol\Comm\zs_b1_can_protocol.c' 'actual_charge_ma\s*=\s*\(snapshot->current_business_ma\s*>\s*0\)\s*\?\s*\(uint32_t\)snapshot->current_business_ma' 'ZS charge current'
Write-Output 'PASS communication current uses post-deadband milliamps'
