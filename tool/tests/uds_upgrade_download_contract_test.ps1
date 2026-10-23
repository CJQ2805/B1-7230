$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bootUds = Join-Path $root 'boot\User\UDS\UDSLogic'
$bootManager = Get-Content -Raw -LiteralPath (Join-Path $bootUds 'uds_upgrade.c')
$bootHeader = Get-Content -Raw -LiteralPath (Join-Path $bootUds 'uds_upgrade.h')
$bootProject = [xml](Get-Content -Raw -LiteralPath (Join-Path $root 'boot\Projects\MDK\boot.uvprojx'))
$appProject = [xml](Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx'))

if ($bootHeader -notmatch 'UDS_UPGRADE_BLOCK_DATA_MAX\s+128U') {
    throw 'BOOT TransferData maximum payload must be 128 bytes.'
}
if ($bootManager -match 'uds_customer_profile_is_flash_driver|s_driver_token|s_marker_failed') {
    throw 'BOOT must not inspect or validate FlashDriver content.'
}
$driverDownload = [regex]::Match(
    $bootManager,
    '(?s)if\s*\(s_state\s*==\s*BOOT_UDS_UPGRADE_IDLE\)(.*?)boot_uds_upgrade_begin_transfer\(BOOT_UDS_TRANSFER_DRIVER')
if (-not $driverDownload.Success -or
    $driverDownload.Groups[1].Value -match 'UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE|BOOT_SRAM_START_ADDR|BOOT_SRAM_END_ADDR') {
    throw 'BOOT FlashDriver download must not require a fixed content size or a SRAM execution range.'
}
if ($bootManager -match 'data_len\s*!=\s*UDS_CUSTOMER_FLASH_DRIVER_ID_SIZE|memcpy\(s_driver_token') {
    throw 'BOOT must receive FlashDriver TransferData without checking or storing its bytes.'
}
$driverCrc = [regex]::Match(
    $bootManager,
    '(?s)if\s*\(s_state\s*==\s*BOOT_UDS_UPGRADE_DRIVER_EXITED\)(.*?)return\s+UDS_UPGRADE_OK\s*;')
if (-not $driverCrc.Success -or
    $driverCrc.Groups[1].Value -match 's_transfer_crc16\s*!=\s*expected_crc16|uds_customer_profile_is_flash_driver' -or
    $driverCrc.Groups[1].Value -notmatch 's_driver_received\s*=\s*1U') {
    throw 'The FlashDriver CRC routine must acknowledge receipt without checking a fixed CRC or token.'
}
if ($bootManager -notmatch '(?s)if\s*\(s_state\s*==\s*BOOT_UDS_UPGRADE_APP_EXITED\)(?:(?!s_state\s*==\s*BOOT_UDS_UPGRADE_DRIVER_EXITED).)*?s_transfer_crc16\s*!=\s*expected_crc16') {
    throw 'BOOT must continue validating the APP transfer CRC.'
}
if ($bootManager -notmatch 'destination\s*=\s*BOOT_APP1_START_ADDR\s*\+\s*s_received_size' -or
    $bootManager -notmatch 'boot_crc32\(\(const uint8_t \*\)BOOT_APP1_START_ADDR') {
    throw 'BOOT must write directly to APP1 and compute its CRC from flash contents.'
}

$bootFiles = $bootProject.SelectNodes('//File')
$bootEntry = $bootFiles | Where-Object { $_.FileName -eq 'uds_upgrade.c' } | Select-Object -First 1
if ($null -eq $bootEntry) { throw 'BOOT project must compile its UDS upgrade manager.' }
$appFiles = $appProject.SelectNodes('//File')
$appEntry = $appFiles | Where-Object { $_.FileName -eq 'uds_upgrade.c' } | Select-Object -First 1
if ($null -eq $appEntry -or $appEntry.FileOption.CommonProperty.IncludeInBuild -ne '0') {
    throw 'APP project must exclude the legacy APP2 UDS upgrade manager.'
}

Write-Output 'BOOT-owned UDS download contract: PASS'
