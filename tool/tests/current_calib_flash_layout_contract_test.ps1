$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$appRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src\App'
$protocolRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\Comm'
$flashMap = Get-Content -Raw -LiteralPath (Join-Path $appRoot 'app_flash_map.h')
$calibration = Get-Content -Raw -LiteralPath (Join-Path $appRoot 'app_current_calib.c')
$header = Get-Content -Raw -LiteralPath (Join-Path $appRoot 'app_current_calib.h')
$can = Get-Content -Raw -LiteralPath (Join-Path $protocolRoot 'ducar_can_protocol.c')
$uart = Get-Content -Raw -LiteralPath (Join-Path $protocolRoot 'ducar_uart_protocol.c')

if ($flashMap -notmatch '#define\s+APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET\s+\(\(uint32_t\)0x00000000UL\)') {
    throw 'Current calibration record must begin at Page0 offset 0x00.'
}
if ($flashMap -notmatch '#define\s+APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET\s+\(\(uint32_t\)0x0000000EUL\)') {
    throw 'Voltage calibration must immediately follow the 14-byte current calibration record at 0x0E.'
}
if ($flashMap -notmatch '#define\s+APP_FLASH_MAP_DB_PARAM_SN_OFFSET\s+\(\(uint32_t\)0x00000054UL\)') {
    throw 'The current-calibration change must preserve the SN offset at 0x54.'
}
if ($flashMap -notmatch '#define\s+APP_FLASH_TAG_CURRENT_CALIB\s+\(\(uint16_t\)0x4355U\)') {
    throw 'Current-calibration marker must be the new U16 ASCII marker UC.'
}
if ($calibration -notmatch '(?s)uint16_t\s+tag;.*?uint32_t\s+k_ppm;.*?int32_t\s+b_ma;.*?int32_t\s+zero_raw_ma;') {
    throw 'Flash record must contain only U16 marker, one shared K, one shared B, and zero raw value.'
}
if ($calibration -match 'charge_k_ppm|discharge_k_ppm|charge_valid|discharge_valid|zero_valid') {
    throw 'Direction-specific calibration state or a mandatory zero-valid gate remains.'
}
if (($calibration + $header) -notmatch 'zero_raw_ma = 0;|0mA') {
    throw 'Optional zero calibration behavior must be documented.'
}
if ($can -notmatch '(?s)case\s+DUCAR_CAN_PGN_F231:.*?app_current_calib_get\s*\(') {
    throw 'CAN F231 must read the shared K/B pair.'
}
if ($uart -notmatch '(?s)ducar_bms_reply_current_calibration.*?app_current_calib_get\s*\(') {
    throw 'UART 0x72 must read the shared K/B pair.'
}

Write-Output 'PASS: Page0 layout, U16 marker, shared K/B, optional zero point, CAN F231, and UART 0x72 contracts.'
