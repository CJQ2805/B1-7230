$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppCanIap = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_can_iap_protocol.c'
$AppCan = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_can.c'
$AppCanHeader = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_can.h'
$UdsPort = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.c'
$CanDriver = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Driver\d_can.c'
$CanDriverHeader = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Driver\d_can.h'
$CanProtocol = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c'
$BootCan = Join-Path $ProjectRoot 'boot\User\Hal_Driver\boot_can.c'
$BootManage = Join-Path $ProjectRoot 'boot\User\Boot\boot_manage.c'

$AppCanIapText = Get-Content -Raw -LiteralPath $AppCanIap
$AppCanText = Get-Content -Raw -LiteralPath $AppCan
$AppCanHeaderText = Get-Content -Raw -LiteralPath $AppCanHeader
$UdsPortText = Get-Content -Raw -LiteralPath $UdsPort
$CanDriverText = Get-Content -Raw -LiteralPath $CanDriver
$CanDriverHeaderText = Get-Content -Raw -LiteralPath $CanDriverHeader
$CanProtocolText = Get-Content -Raw -LiteralPath $CanProtocol
$BootCanText = Get-Content -Raw -LiteralPath $BootCan
$BootManageText = Get-Content -Raw -LiteralPath $BootManage

if ($AppCanIapText -notmatch 'DUCAR_CAN_IAP_TIMEOUT_MS\s+\(\(uint32_t\)10000UL\)') {
    throw 'APP CAN IAP must abort an inactive upgrade session after ten seconds.'
}

$TransmitTaskStart = $AppCanText.IndexOf('void CAN_TransmitTask(void)')
$SleepGateStart = $AppCanText.IndexOf('bool app_can_is_sleep_allowed(void)', $TransmitTaskStart)
if (($TransmitTaskStart -lt 0) -or ($SleepGateStart -lt 0)) {
    throw 'Cannot locate the APP CAN transmit scheduler.'
}
$TransmitTaskText = $AppCanText.Substring($TransmitTaskStart, $SleepGateStart - $TransmitTaskStart)
if ($TransmitTaskText -notmatch 'if\s*\(\s*u8IapBusy\s*==\s*0U\s*\)\s*\{\s*ducar_can_control_reply_task\s*\(\s*\)\s*;') {
    throw 'APP CAN must stop queued normal replies while IAP is active.'
}

if ($TransmitTaskText -notmatch 'if\s*\(\s*\(u8IapBusy\s*==\s*0U\)\s*&&[\s\S]*?\)\s*\{[\s\S]*?ducar_can_50ms_task\s*\(\s*\)') {
    throw 'APP CAN must stop periodic normal frames while IAP is active.'
}

$ReceiveGate = [regex]::Match(
    $AppCanText,
    'ducar_can_iap_protocol_is_busy\(\)\s*&&\s*\(eCanIdType\s*!=\s*APP_CAN_ID_DUCAR_IAP\)\s*&&\s*\(eCanIdType\s*!=\s*APP_CAN_ID_ECM_STATUS6\)\s*\)\s*\{\s*continue;')
if (-not $ReceiveGate.Success) {
    throw 'APP CAN must ignore non-IAP requests except the 0x105 KEY heartbeat while IAP is active.'
}

if ($AppCanText -notmatch 'ducar_can_protocol_set_iap_silence\(u8IapBusy\s*!=\s*0U\)') {
    throw 'APP CAN must clear stale normal replies on entry to the IAP silent window.'
}

if ($AppCanText -notmatch 'ducar_can_iap_protocol_receive\(&tCanPacket\);[\s\S]{0,180}ducar_can_iap_protocol_is_busy\(\)[\s\S]{0,180}ducar_can_protocol_set_iap_silence\(true\)') {
    throw 'APP CAN must enter the low-level transmit silence as soon as an IAP frame starts a session.'
}

if (($CanDriverHeaderText -notmatch 'd_can_send_iap\s*\(') -or
    ($CanDriverHeaderText -notmatch 'd_can_set_iap_silence\s*\(')) {
    throw 'CAN driver must separate IAP responses from normal traffic during the silent window.'
}

if (($AppCanIapText -notmatch 'd_can_send_iap\s*\(') -or
    ($CanProtocolText -notmatch 'd_can_set_iap_silence\(enabled\)')) {
    throw 'IAP frames must remain transmittable while ordinary CAN is silenced.'
}

if (($CanDriverText -notmatch 'CAN_CancelTransmitMessage\(CAN1,\s*u8Mailbox\)') -or
    ($CanDriverText -notmatch 's_can_iap_silence_active') -or
    ($CanDriverText -notmatch 'is_iap') -or
    ($CanDriverText -notmatch 's_can_iap_silence_active\s*!=\s*false\)\s*&&\s*\(is_iap\s*==\s*0U\)') -or
    ($CanDriverText -notmatch 's_can_iap_silence_active\s*==\s*false\)\s*\|\|\s*\(s_can_tx_mailbox_state\[u8Mailbox\]\.is_iap\s*!=\s*0U\)')) {
    throw 'CAN driver must cancel pending ordinary mailboxes and prevent their retries during IAP.'
}

if (($AppCanText + $AppCanHeaderText) -notmatch 'app_can_iap_is_busy\s*\(') {
    throw 'APP CAN must expose IAP state to the independent UDS CAN sender.'
}

$UdsSendStart = $UdsPortText.IndexOf('int uds_send_frame(')
$UdsSendEnd = $UdsPortText.IndexOf('void uds_request_reset_after_tx(', $UdsSendStart)
if (($UdsSendStart -lt 0) -or ($UdsSendEnd -lt 0)) {
    throw 'Cannot locate the UDS CAN send adapter.'
}
$UdsSendText = $UdsPortText.Substring($UdsSendStart, $UdsSendEnd - $UdsSendStart)
if (($UdsSendText -notmatch 'app_can_iap_is_busy\(\)') -or
    ($UdsSendText -notmatch 'return\s+-1\s*;')) {
    throw 'UDS CAN frames must be refused while CAN IAP owns the bus.'
}

if ($AppCanIapText -notmatch 'DUCAR_CAN_IAP_STATE_RECEIVING_PAGE') {
    throw 'APP CAN IAP must own the TP receiving state.'
}

if ($AppCanIapText -match 'ducar_iap_prepare_boot_wait_package\(') {
    throw 'E1/02 must not enter BOOT wait-package mode.'
}

if ($AppCanIapText -notmatch 'ducar_iap_prepare_boot_move\(') {
    throw 'APP CAN IAP must request BOOT move only after complete validation.'
}

if ($BootCanText -notmatch 'boot_can_upgrade_init\(') {
    throw 'BOOT CAN driver must keep its standalone CAN upgrade initialization.'
}

if ($BootCanText -notmatch 'boot_can_upgrade_task\(' -or
    $BootCanText -notmatch 'boot_can_upgrade_on_receive\(') {
    throw 'BOOT CAN driver must keep its standalone CAN upgrade dispatch.'
}

if ($BootManageText -notmatch 'BOOT_UPGRADE_FLAG_NEED_MOVE') {
    throw 'BOOT manager must keep the explicit CCCC move state.'
}

if ($BootManageText -notmatch 'BOOT_UPGRADE_FLAG_WAIT_PACKAGE') {
    throw 'BOOT manager must fall back to BBBB when move fails.'
}

Write-Output 'CAN IAP flow contract: PASS'
