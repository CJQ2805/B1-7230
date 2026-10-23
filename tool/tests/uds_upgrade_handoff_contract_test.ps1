$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$base = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase'
$logic = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic'
$ecuReset = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID11_EcuReset.c')
$upgrade = Get-Content -Raw -LiteralPath (Join-Path $logic 'uds_upgrade.c')
$port = Get-Content -Raw -LiteralPath (Join-Path $base 'uds_port.c')
$tp = Get-Content -Raw -LiteralPath (Join-Path $base 'uds_tp.c')
$service = Get-Content -Raw -LiteralPath (Join-Path $base 'uds_service.c')

if ($ecuReset -notmatch 'UDS_RESET_SOFT' -or
    $ecuReset -notmatch '(?s)if\s*\(!uds_upgrade_image_verified\(\)\).*?NRC_CONDITIONS_NOT_CORRECT.*?uds_upgrade_prepare_boot_move') {
    throw '11 03 must be denied until CRC16 and dependency checks have passed'
}
if ($ecuReset -notmatch '(?s)uds_positive_rsp\(rsp_buf,\s*sizeof\(rsp_buf\)\)\s*==\s*0.*?uds_request_reset_after_tx\(\)') {
    throw 'Reset may only be armed after the 51 03 positive response was submitted'
}
if ($ecuReset -match 'd_system_reset\s*\(') {
    throw 'ECU reset must not execute inside the SID11 service callback'
}
if ($upgrade -notmatch '(?s)if\s*\(actual_crc16\s*==\s*expected_crc16\).*?UDS_UPGRADE_STATE_DRIVER_VERIFIED.*?else.*?UDS_UPGRADE_STATE_IDLE') {
    throw 'A bad BMS-DUCAR CRC16 must not advance the protocol to the erase stage'
}
if ($upgrade -notmatch '(?s)if\s*\(s_state\s*!=\s*UDS_UPGRADE_STATE_DEPENDENCIES_VERIFIED\).*?return\s+FALSE.*?ducar_iap_prepare_boot_move') {
    throw 'BOOT move metadata must not be prepared before successful whole-image CRC verification'
}
if ($port -notmatch '(?s)network_task\(\);\s*service_task\(\);.*?s_uds_reset_after_tx\s*!=\s*0U.*?d_can_is_tx_idle\(&can0\).*?d_system_reset\(\)') {
    throw 'Deferred reset must wait for the CAN TX mailboxes to drain in the 1 ms task'
}
if ($tp -notmatch '(?s)static int send_singleframe.*?return uds_send_frame\(RESPONSE_ID,\s*send_buf,\s*FRAME_SIZE\).*?if \(msg_dlc < FRAME_SIZE\).*?return send_singleframe\(msg_buf,\s*msg_dlc\)') {
    throw 'Single-frame response submission status must propagate back to SID11'
}
if ($service -notmatch '(?s)uds_timer_run\(UDS_TIMER_S3server\)\s*<\s*0.*?uds_upgrade_reset_state\(\).*?service_28_reset\(\).*?service_85_reset\(\)') {
    throw 'S3 timeout must reset download, CommunicationControl, and DTC setting states'
}
Write-Output 'UDS upgrade handoff contract: PASS'
