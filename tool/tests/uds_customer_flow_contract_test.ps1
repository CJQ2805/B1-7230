$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$app = Join-Path $root 'app\N32G45x_KEIL'
$logic = Join-Path $app 'User_Src\Protocol\UDS\UDSLogic'
$base = Join-Path $app 'User_Src\Protocol\UDS\UDSBase'
$serviceHeader = Get-Content -Raw -LiteralPath (Join-Path $logic 'service_cfg.h')
$serviceConfig = Get-Content -Raw -LiteralPath (Join-Path $logic 'service_cfg.c')
$project = [xml](Get-Content -Raw -LiteralPath (Join-Path $app 'MDK-ARM\N32G45x.uvprojx'))

if ($serviceHeader -notmatch '#define\s+SID_NUM\s+12') {
    throw 'Customer UDS profile must register its 12 implemented services.'
}
foreach ($sid in 'SID_10', 'SID_11', 'SID_22', 'SID_27', 'SID_2E', 'SID_28',
    'SID_31', 'SID_34', 'SID_36', 'SID_37', 'SID_3E', 'SID_85') {
    if ($serviceConfig -notmatch ('\{' + $sid + '\s*,')) {
        throw "Customer UDS service is not registered: $sid"
    }
}

$transferExit = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID37_RequestTransferExit.c')
if ($transferExit -notmatch 'msg_dlc\s*==\s*1U' -or
    $transferExit -notmatch 'uds_upgrade_transfer_exit\s*\(') {
    throw 'Customer RequestTransferExit must be SID 37 with no payload and close the active transfer.'
}

$routine = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID31_RoutineControl.c')
foreach ($rid in '0x0202', '0xFF00', '0xFF01') {
    if ($routine -notmatch [regex]::Escape($rid)) { throw "RoutineControl RID missing: $rid" }
}
if ($routine -notmatch 'uds_upgrade_validate_transfer_crc16' -or
    $routine -notmatch 'uds_upgrade_erase_application' -or
    $routine -notmatch 'uds_upgrade_check_programming_dependencies') {
    throw 'RoutineControl must validate CRC16, erase the requested APP block, and check dependencies.'
}

$reset = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID11_EcuReset.c')
if ($reset -notmatch 'UDS_RESET_SOFT' -or $reset -notmatch '51') {
    throw 'Customer programming completion must use ECUReset 11 03 / positive response 51 03.'
}

$readDid = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID22_ReadDataByIdentifier.c')
foreach ($did in '0xF180', '0xF18A', '0xF193', '0xF189') {
    if ($readDid -notmatch [regex]::Escape($did)) { throw "Customer DID missing: $did" }
}

$writeDid = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID2E_WriteDataByIdentifier.c')
if ($writeDid -notmatch '0xF184' -or
    $writeDid -notmatch 'uds_upgrade_store_fingerprint' -or
    $writeDid -notmatch 'UDS_CUSTOMER_FINGERPRINT_SIZE') {
    throw 'F184 must persist the eight-byte fingerprint with the approved two-byte tag.'
}

$upgradeHeader = Get-Content -Raw -LiteralPath (Join-Path $logic 'uds_upgrade.h')
$upgrade = Get-Content -Raw -LiteralPath (Join-Path $logic 'uds_upgrade.c')
if ($upgradeHeader -notmatch 'uds_upgrade_validate_transfer_crc16' -or
    $upgrade -notmatch 'APP_FLASH_MAP_APP2_START_ADDR' -or
    $upgrade -notmatch 'uds_customer_profile_is_flash_driver' -or
    $upgrade -notmatch 'uds_customer_profile_crc16' -or
    $upgrade -notmatch 'APP_FLASH_TAG_UDS_DOWNLOAD_FINGERPRINT') {
    throw 'Upgrade manager must recognize BMS-DUCAR, CRC16-check each transfer, and stage APP into APP2.'
}

$appCan = Get-Content -Raw -LiteralPath (Join-Path $app 'User_Src\App\app_can.c')
$canDriver = Get-Content -Raw -LiteralPath (Join-Path $app 'User_Src\Driver\d_can.c')
$commControl = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID28_CommunicationControl.c')
if ($appCan -notmatch 'uds_normal_receive_enabled' -or
    $commControl -notmatch 'd_can_set_extended_tx_enabled' -or
    $canDriver -notmatch 'D_CAN_ID_EXTENDED\) && !s_extended_tx_enabled') {
    throw 'CommunicationControl must gate normal CAN receive and transmit while leaving UDS active.'
}

$projectFiles = $project.SelectNodes('//File')
foreach ($fileName in 'uds_customer_profile.c', 'uds_upgrade.c',
    'SID2E_WriteDataByIdentifier.c', 'SID28_CommunicationControl.c',
    'SID31_RoutineControl.c', 'SID85_ControlDTCSetting.c') {
    $entry = $projectFiles | Where-Object { $_.FileName -eq $fileName } | Select-Object -First 1
    if ($null -eq $entry -or $entry.FileOption.CommonProperty.IncludeInBuild -ne '1') {
        throw "Keil application build must include $fileName"
    }
}

Write-Output 'Customer UDS flow contract: PASS'
