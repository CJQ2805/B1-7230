$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$uds = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic'
$managerPath = Join-Path $uds 'uds_upgrade.c'
$managerHeaderPath = Join-Path $uds 'uds_upgrade.h'
if (!(Test-Path -LiteralPath $managerPath) -or !(Test-Path -LiteralPath $managerHeaderPath)) {
    throw 'UDS upgrade state manager is missing'
}

$manager = Get-Content -Raw -LiteralPath $managerPath
$managerHeader = Get-Content -Raw -LiteralPath $managerHeaderPath
$requestDownload = Get-Content -Raw -LiteralPath (Join-Path $uds 'SID34_RequestDownload.c')
$transferData = Get-Content -Raw -LiteralPath (Join-Path $uds 'SID36_TransferData.c')
$transferExit = Get-Content -Raw -LiteralPath (Join-Path $uds 'SID37_RequestTransferExit.c')
$project = [xml](Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx'))

foreach ($token in 'APP_FLASH_MAP_APP1_START_ADDR', 'APP_FLASH_MAP_APP2_START_ADDR',
    'APP_FLASH_MAP_APP_CODE_SIZE', 'D_FLASH_PAGE_SIZE', 'ducar_iap_crc32_calc',
    'ducar_iap_prepare_boot_move') {
    if ($manager -notmatch [regex]::Escape($token)) { throw "UDS upgrade manager contract missing: $token" }
}
if ($managerHeader -notmatch 'UDS_UPGRADE_BLOCK_DATA_MAX\s+128U') {
    throw 'UDS TransferData maximum payload must be 128 bytes'
}
if ($manager -notmatch '(?s)static bool_t uds_upgrade_duplicate_matches.*?memcmp\(data,\s*previous_data,\s*data_len\).*?uds_upgrade_duplicate_matches\(block_counter,\s*data,\s*data_len\)') {
    throw 'A retransmitted last block must be acknowledged idempotently without programming it twice'
}
if ($requestDownload -notmatch '(?s)msg_dlc\s*!=\s*11U.*?msg_buf\[1\]\s*!=\s*0x00U.*?msg_buf\[2\]\s*!=\s*0x44U.*?sid34_read_u32_be\(&msg_buf\[3\]\).*?uds_upgrade_request_download') {
    throw 'RequestDownload must validate the agreed 11-byte big-endian address/size request'
}
if ($requestDownload -notmatch '(?s)rsp_buf\[1\]\s*=\s*0x20.*?rsp_buf\[2\]\s*=\s*0x00.*?rsp_buf\[3\]\s*=\s*0x82') {
    throw 'RequestDownload must advertise maxBlockLength 0x0082 (130-byte UDS PDU)'
}
if ($transferData -notmatch '(?s)msg_dlc\s*>=\s*3U.*?uds_upgrade_transfer_data\(block_counter,\s*&msg_buf\[2\],\s*\(uint16_t\)\(msg_dlc\s*-\s*2U\)\)') {
    throw 'TransferData must pass the BSC and only actual image bytes to the state manager'
}
if ($transferExit -notmatch '(?s)msg_dlc\s*==\s*1U.*?uds_upgrade_transfer_exit') {
    throw 'Customer RequestTransferExit must be SID 37 with no payload'
}
if ($manager -notmatch 'uds_upgrade_validate_transfer_crc16' -or
    $manager -notmatch 'UDS_UPGRADE_STATE_DRIVER_EXITED' -or
    $manager -notmatch 'UDS_UPGRADE_STATE_APP_EXITED') {
    throw 'CRC16 verification must be separate from SID 37 for both FlashDriver and APP transfers'
}

$files = $project.SelectNodes('//File')
$entry = $files | Where-Object { $_.FileName -eq 'uds_upgrade.c' } | Select-Object -First 1
if ($null -eq $entry -or $entry.FileOption.CommonProperty.IncludeInBuild -ne '1') {
    throw 'UDS upgrade state manager must be included in the Keil application build'
}
Write-Output 'UDS upgrade download contract: PASS'
