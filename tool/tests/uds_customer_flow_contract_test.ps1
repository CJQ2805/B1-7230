$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$app = Join-Path $root 'app\N32G45x_KEIL'
$logic = Join-Path $app 'User_Src\Protocol\UDS\UDSLogic'
$base = Join-Path $app 'User_Src\Protocol\UDS\UDSBase'
$serviceHeader = Get-Content -Raw -LiteralPath (Join-Path $logic 'service_cfg.h')
$serviceConfig = Get-Content -Raw -LiteralPath (Join-Path $logic 'service_cfg.c')
$project = [xml](Get-Content -Raw -LiteralPath (Join-Path $app 'MDK-ARM\N32G45x.uvprojx'))

if ($serviceHeader -notmatch '#define\s+SID_NUM\s+5') {
    throw 'APP UDS must register only the services needed before BOOT takes over.'
}
foreach ($sid in 'SID_10', 'SID_22', 'SID_27', 'SID_28', 'SID_85') {
    if ($serviceConfig -notmatch ('\{' + $sid + '\s*,')) {
        throw "Customer UDS service is not registered: $sid"
    }
}

$session = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID10_SessionControl.c')
if ($session -notmatch 'ducar_iap_prepare_boot_wait_package\(\)' -or
    $session -notmatch 'uds_request_reset_after_tx\(\)' -or
    $session -notmatch 'ducar_iap_cancel_boot_wait_package\(\)') {
    throw 'APP must arm BOOT wait-package mode and reset only after the 50 02 response is queued.'
}

$readDid = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID22_ReadDataByIdentifier.c')
foreach ($did in '0xF180', '0xF18A', '0xF193', '0xF189') {
    if ($readDid -notmatch [regex]::Escape($did)) { throw "Customer DID missing: $did" }
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
foreach ($fileName in 'uds_customer_profile.c', 'SID10_SessionControl.c',
    'SID22_ReadDataByIdentifier.c', 'SID27_SecurityAccess.c',
    'SID28_CommunicationControl.c', 'SID85_ControlDTCSetting.c') {
    $entry = $projectFiles | Where-Object { $_.FileName -eq $fileName } | Select-Object -First 1
    if ($null -eq $entry -or $entry.FileOption.CommonProperty.IncludeInBuild -ne '1') {
        throw "Keil application build must include $fileName"
    }
}

foreach ($fileName in 'uds_upgrade.c', 'SID11_EcuReset.c', 'SID3E_TesterPresent.c',
    'SID2E_WriteDataByIdentifier.c', 'SID31_RoutineControl.c',
    'SID34_RequestDownload.c', 'SID36_TransferData.c',
    'SID37_RequestTransferExit.c') {
    $entry = $projectFiles | Where-Object { $_.FileName -eq $fileName } | Select-Object -First 1
    if ($null -eq $entry -or $entry.FileOption.CommonProperty.IncludeInBuild -ne '0') {
        throw "APP must leave BOOT-owned upgrade source out of its build: $fileName"
    }
}

Write-Output 'Customer UDS flow contract: PASS'
