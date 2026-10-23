$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$SourcePath = Join-Path $ProjectRoot 'app/N32G45x_KEIL/User_Src/Protocol/IAP/ducar_can_iap_protocol.c'
$SourceText = Get-Content -Raw $SourcePath

$SendStateStart = $SourceText.IndexOf('if (eCanIapTxState == DUCAR_CAN_IAP_TX_PENDING)')
$WaitStateStart = $SourceText.IndexOf('if (eCanIapTxState == DUCAR_CAN_IAP_TX_WAIT_IDLE)', $SendStateStart)

if (($SendStateStart -lt 0) -or ($WaitStateStart -lt 0)) {
    throw 'Cannot locate the CAN IAP SEND and WAIT_TX state handlers.'
}

$SendStateBody = $SourceText.Substring($SendStateStart, $WaitStateStart - $SendStateStart)
$TimeoutBranch = [regex]::Match(
    $SendStateBody,
    'else\s+if\s*\(\s*\+\+u8CanIapTxRetryTicks\s*>=\s*DUCAR_CAN_IAP_TX_RETRY_TICKS\s*\)\s*\{(?<Body>[^{}]*)\}')
$HasBoundedRetry = $TimeoutBranch.Success -and
    ($TimeoutBranch.Groups['Body'].Value -match 'ducar_can_iap_clear_tx_state\s*\(\s*\)') -and
    ($TimeoutBranch.Groups['Body'].Value -match 'ducar_can_iap_reset_transfer_state\s*\(\s*\)')

if (-not $HasBoundedRetry) {
    throw 'CAN IAP SEND state has no finite timeout when d_can_send() cannot queue the ACK.'
}

Write-Output 'PASS: CAN IAP SEND state bounds ACK queue retries.'

$NormalProtocolPath = Join-Path $ProjectRoot 'app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c'
$NormalProtocolText = Get-Content -Raw $NormalProtocolPath
$ControlReplyStart = $NormalProtocolText.IndexOf('void ducar_can_control_reply_task(void)')
$PendingAckStart = $NormalProtocolText.IndexOf('if (u8CanAckPending != 0U)', $ControlReplyStart)

if (($ControlReplyStart -lt 0) -or ($PendingAckStart -lt 0)) {
    throw 'Cannot locate the normal CAN pending-ACK handler.'
}

$PendingAckBody = $NormalProtocolText.Substring($PendingAckStart)
$AckTimeoutBranch = [regex]::Match(
    $PendingAckBody,
    'else\s+if\s*\(\s*\+\+u8CanActionWaitTicks\s*>=\s*DUCAR_CAN_ACK_WAIT_MAX_TICKS\s*\)\s*\{(?<Body>[^{}]*)\}')
$HasBoundedAckQueue = $AckTimeoutBranch.Success
$ClearsUnsentAction = $HasBoundedAckQueue -and
    ($AckTimeoutBranch.Groups['Body'].Value -match 'u8CanAckPending\s*=\s*0U;') -and
    ($AckTimeoutBranch.Groups['Body'].Value -match 'eCanActionAfterAck\s*=\s*DUCAR_CAN_ACTION_NONE;')

if (-not $HasBoundedAckQueue -or -not $ClearsUnsentAction) {
    throw 'Normal CAN ACK queue retry is unbounded or leaves an action pending after ACK timeout.'
}

Write-Output 'PASS: normal CAN pending ACK timeout releases reply state without running its action.'
