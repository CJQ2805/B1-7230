$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$can = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c')
$uart = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_uart_protocol.c')
$flashMap = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_flash_map.h')
$bms = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Task\bms_task.c')
$saveLogTask = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\task\save_log_task.c')
$project = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx')
$main = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\Core\src\main.c')
$param = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_param.c')
$calib = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_current_calib.c')

foreach ($pgn in 'f219','f21a','f21b','f21c','f21d','f21e','f21f') {
    if ($can -notmatch "ducar_can_send_$pgn\(\)") { throw "Missing A0.2 sender $pgn" }
}
foreach ($token in 'DUCAR_CAN_PGN_F230','DUCAR_CAN_PGN_F231','DUCAR_CAN_PGN_F232',
                  'app_param_set_manufacture_sn','app_current_calib_get',
                  'app_current_calib_submit','app_hw_apply_clear_all_afe_fault',
                  'app_bms_force_soc_ocv') {
    if ($can -notmatch $token) { throw "Missing A0.2 path $token" }
}
if ($uart -notmatch 'ducar_bms_pack_log_payload') { throw 'CAN log encoder does not reuse UART 0x49 body' }
if ($can -match 'ducar_can_send_f22d\(') { throw 'Old F22D active alarm frame remains' }

if ($flashMap -notmatch 'APP_FLASH_MAP_DB_SOC_ADDR') { throw 'Missing fixed SOC Flash page in the application database map' }
if ($flashMap -match 'APP_FLASH_MAP_SOC_STORE_START_ADDR|APP_FLASH_MAP_SOC_STORE_PAGE_COUNT') {
    throw 'Obsolete SOC wear-level Flash partition remains in the map'
}
foreach ($token in 'APP_FLASH_TAG_SOC','d_flash_erase_page\(APP_FLASH_MAP_DB_SOC_ADDR\)',
                  'd_flash_write\(APP_FLASH_MAP_DB_SOC_ADDR','app_bms_soc_flash_restore\(\)',
                  'memcmp\(') {
    if ($bms -notmatch $token) { throw "Missing direct SOC Flash behavior in BMS task: $token" }
}
if ($bms -match 'app_soc_store|APP_BMS_SOC_WRITE_WAIT_FLASH') {
    throw 'Legacy SOC store queue/state-machine remains in BMS task'
}
if ($bms -notmatch 'app_bms_soc_flash_save\(\)') { throw 'BMS task does not directly persist SOC changes' }
if ($bms -notmatch 'app_soc_set_requested_percent\(requested_soc\)') { throw 'E200 does not apply the host-requested SOC' }
if ($saveLogTask -match 'app_soc_store') { throw 'SaveLog task still services the removed SOC store' }
foreach ($path in 'app\N32G45x_KEIL\User_Src\App\app_soc_store.c',
                  'app\N32G45x_KEIL\User_Src\App\app_soc_store.h') {
    if (Test-Path -LiteralPath (Join-Path $root $path)) { throw "Removed SOC store file still exists: $path" }
}
$sn = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_param.c')
if ($flashMap -match '#define\s+APP_FLASH_MAP_DB_MANUFACTURE_SN_ADDR\s') {
    throw 'SN still owns a dedicated Flash page instead of sharing the parameter page'
}
foreach ($token in 'APP_FLASH_MAP_DB_PARAM_SN_OFFSET','APP_FLASH_TAG_MANUFACTURE_SN') {
    if ($flashMap -notmatch $token) { throw "Missing shared SN map entry: $token" }
}
foreach ($token in 'app_param_flash_page_update\(','APP_FLASH_MAP_DB_PARAM_SN_OFFSET',
                  'APP_FLASH_TAG_MANUFACTURE_SN','app_param_sn_record_is_cleared',
                  'APP_PARAM_PRODUCT_MODE_PRODUCTION') {
    if ($param -notmatch $token) { throw "Missing shared-page parameter persistence: $token" }
}
# Current test firmware deliberately has no legacy-SN migration or migration marker.
if (($flashMap + $param) -match 'APP_FLASH_MAP_DB_PARAM_SN_MIGRATION_OFFSET|APP_FLASH_MAP_DB_LEGACY_MANUFACTURE_SN_ADDR|APP_PARAM_SN_MIGRATION_MAGIC|app_param_sn_migration_is_complete') {
    throw 'Removed legacy-SN migration must not be reintroduced'
}
foreach ($token in 's_app_param_flash_page_buffer','pu32FlashPage\[u32WordIndex\]',
                  'memcpy\(','d_flash_erase_page\(APP_FLASH_MAP_DB_PARAM_ADDR\)',
                  'd_flash_write\(APP_FLASH_MAP_DB_PARAM_ADDR','pu32FlashPage\[u32WordIndex\] != s_app_param_flash_page_buffer') {
    if ($param -notmatch $token) { throw "Shared parameter page is not fully read-modify-written and verified: $token" }
}
if ($param -match 'd_flash_erase_page\(APP_FLASH_MAP_DB_MANUFACTURE_SN_ADDR\)|d_flash_write\(APP_FLASH_MAP_DB_MANUFACTURE_SN_ADDR') {
    throw 'SN code still erases or writes a dedicated Flash page'
}
if ($calib -notmatch 'app_param_flash_page_update\(APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET') {
    throw 'Calibration save does not preserve other data in the shared parameter page'
}
if (($main.IndexOf('d_flash_init()') -lt 0) -or ($main.IndexOf('app_param_init()') -lt 0) -or
    ($main.IndexOf('d_flash_init()') -gt $main.IndexOf('app_param_init()'))) {
    throw 'Flash guard is not initialized before shared parameter page access'
}
if ($project -match 'app_soc_store\.c|app_soc_store\.h') { throw 'Keil project still references removed SOC store files' }
Write-Output 'CAN A0.2 contract: PASS'
