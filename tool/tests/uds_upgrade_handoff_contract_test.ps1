$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$appLogic = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic'
$appBase = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase'
$appProject = [xml](Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx'))
$bootLogic = Join-Path $root 'boot\User\UDS\UDSLogic'
$appSession = Get-Content -Raw -LiteralPath (Join-Path $appLogic 'SID10_SessionControl.c')
$appService = Get-Content -Raw -LiteralPath (Join-Path $appBase 'uds_service.c')
$appPort = Get-Content -Raw -LiteralPath (Join-Path $appBase 'uds_port.c')
$bootUds = Get-Content -Raw -LiteralPath (Join-Path $bootLogic 'boot_uds.c')
$bootUpgrade = Get-Content -Raw -LiteralPath (Join-Path $bootLogic 'uds_upgrade.c')
$bootServices = Get-Content -Raw -LiteralPath (Join-Path $bootLogic 'service_cfg.c')
$projectFiles = $appProject.SelectNodes('//File')

if ($appSession -notmatch 'ducar_iap_prepare_boot_wait_package\(\)' -or
    $appSession -notmatch '(?s)uds_positive_rsp\(rsp_buf,\s*6U\)\s*==\s*0.*?uds_request_reset_after_tx\(\)' -or
    $appSession -notmatch 'ducar_iap_cancel_boot_wait_package\(\)') {
    throw 'APP must arm BOOT wait-package mode and reset only after the 50 02 response is queued.'
}
if (($appSession + $appService) -match 'uds_upgrade_(reset_state|image_verified|prepare_boot_move)\s*\(') {
    throw 'APP session and service timeout paths must not retain the legacy APP2 download state machine.'
}
if ($appPort -notmatch '(?s)s_uds_reset_after_tx\s*!=\s*0U.*?d_can_is_tx_idle\(&can0\).*?d_system_reset\(\)') {
    throw 'APP handoff reset must wait until the positive response has drained from CAN.'
}
foreach ($name in 'uds_upgrade.c', 'SID11_EcuReset.c', 'SID2E_WriteDataByIdentifier.c',
    'SID31_RoutineControl.c', 'SID34_RequestDownload.c', 'SID36_TransferData.c',
    'SID37_RequestTransferExit.c', 'SID3E_TesterPresent.c') {
    $entry = $projectFiles | Where-Object { $_.FileName -eq $name } | Select-Object -First 1
    if ($null -eq $entry -or $entry.FileOption.CommonProperty.IncludeInBuild -ne '0') {
        throw "APP must leave BOOT-owned upgrade source out of its build: $name"
    }
}
if ($bootUds -notmatch '(?s)boot_upgrade_get_flag\(\)\s*!=\s*BOOT_UPGRADE_FLAG_WAIT_PACKAGE.*?return\s+false.*?uds_init\(\)') {
    throw 'BOOT UDS must start only after APP has set WAIT_PACKAGE, or BOOT detects an invalid APP.'
}
foreach ($sid in 'SID_2E', 'SID_31', 'SID_34', 'SID_36', 'SID_37') {
    if ($bootServices -notmatch ('\{' + $sid + '\s*,')) {
        throw "BOOT must retain upgrade service $sid after it takes over."
    }
}
if ($bootUpgrade -notmatch 'destination\s*=\s*BOOT_APP1_START_ADDR\s*\+\s*s_received_size') {
    throw 'BOOT must write transferred APP bytes directly to APP1.'
}

Write-Output 'APP-to-BOOT UDS handoff contract: PASS'
