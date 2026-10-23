$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppMapPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_flash_map.h'
$AppCanIapPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_can_iap_protocol.c'
$AppUartIapPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_uart_iap_protocol.c'
$AppUartHeaderPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_uart_iap_protocol.h'
$BootConfigPath = Join-Path $ProjectRoot 'boot\User\Boot\boot_config.h'
$BootUpgradePath = Join-Path $ProjectRoot 'boot\User\Boot\boot_upgrade.c'
$BootUpgradeHeaderPath = Join-Path $ProjectRoot 'boot\User\Boot\boot_upgrade.h'
$BootProtocolPath = Join-Path $ProjectRoot 'boot\User\Boot\boot_protocol.c'
$BootCanPath = Join-Path $ProjectRoot 'boot\User\Boot\boot_can_upgrade.c'

$AppMapText = Get-Content -Raw -LiteralPath $AppMapPath
$AppCanIapText = Get-Content -Raw -LiteralPath $AppCanIapPath
$AppUartIapText = Get-Content -Raw -LiteralPath $AppUartIapPath
$AppUartHeaderText = Get-Content -Raw -LiteralPath $AppUartHeaderPath
$BootConfigText = Get-Content -Raw -LiteralPath $BootConfigPath
$BootUpgradeText = Get-Content -Raw -LiteralPath $BootUpgradePath
$BootUpgradeHeaderText = Get-Content -Raw -LiteralPath $BootUpgradeHeaderPath
$BootProtocolText = Get-Content -Raw -LiteralPath $BootProtocolPath
$BootCanText = Get-Content -Raw -LiteralPath $BootCanPath

if ($AppMapText -notmatch 'APP_FLASH_MAP_BOOT_UPGRADE_INFO_ADDR') {
    throw 'APP flash map must reserve a shared BOOT upgrade metadata address.'
}

if (($AppMapText -notmatch 'APP_FLASH_MAP_BOOT_UPGRADE_INFO_MAGIC') -or
    ($BootConfigText -notmatch 'BOOT_UPGRADE_INFO_MAGIC')) {
    throw 'APP and BOOT must use the same upgrade metadata magic.'
}

if (($AppUartHeaderText -notmatch 'ducar_iap_prepare_boot_move\s*\(\s*uint32_t') -or
    ($AppUartIapText -notmatch 'ducar_iap_prepare_boot_move\s*\(\s*uint32_t') -or
    ($AppUartIapText -notmatch 'image_size') -or
    ($AppUartIapText -notmatch 'image_crc32')) {
    throw 'APP must persist image length and CRC before writing the CCCC move flag.'
}

if ($AppCanIapText -notmatch 'ducar_iap_prepare_boot_move\s*\(\s*tCanIapFirmwareInfo\.size\s*,\s*tCanIapFirmwareInfo\.crc32\s*\)') {
    throw 'CAN APP IAP must pass the received firmware length and CRC to BOOT metadata preparation.'
}

if (($BootUpgradeHeaderText -notmatch 'boot_upgrade_prepare_move\s*\(') -or
    ($BootUpgradeText -notmatch 'boot_upgrade_prepare_move\s*\(') -or
    ($BootUpgradeText -notmatch 'boot_upgrade_get_move_info\s*\(')) {
    throw 'BOOT must expose metadata write/read helpers for the pending move.'
}

if ($BootUpgradeText -notmatch 'image_size.*BOOT_FLASH_PAGE_SIZE') {
    throw 'BOOT copy length must be derived from the persisted image size.'
}

if (($BootProtocolText -notmatch 'boot_upgrade_prepare_move\s*\(') -or
    ($BootCanText -notmatch 'boot_upgrade_prepare_move\s*\(')) {
    throw 'UART and standalone BOOT CAN upgrade paths must both persist move metadata.'
}

Write-Output 'BOOT move length contract: PASS'
