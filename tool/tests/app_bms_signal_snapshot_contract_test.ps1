$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '../..')
$typesPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_types.h'
$snapshotPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_snapshot.c'
$typesSource = Get-Content -Raw $typesPath
$snapshotSource = Get-Content -Raw $snapshotPath

$signalTypeMatch = [regex]::Match(
    $typesSource,
    '(?s)typedef\s+struct\s*\{(?<fields>.*?)\}\s*app_bms_signal_t\s*;')
if (-not $signalTypeMatch.Success) {
    throw 'Could not find app_bms_signal_t.'
}

$signalFields = $signalTypeMatch.Groups['fields'].Value
if ($signalFields -notmatch 'app_input_signals_t\s+input\s*;') {
    throw 'The signal snapshot must retain the final external input results.'
}

foreach ($obsoleteField in @('load_removed', 'comm_active', 'uart_rx_count', 'uart_tx_count', 'uart_tx_pending')) {
    if ($signalFields -match "\b$obsoleteField\b") {
        throw "Unused snapshot field '$obsoleteField' must be removed."
    }
}

if ($snapshotSource -notmatch 'snapshot->signal\.input\s*=\s*gt_app_input_signals\s*;') {
    throw 'Snapshot refresh must continue copying the final external input results.'
}

$signalUpdateMatch = [regex]::Match(
    $snapshotSource,
    '(?s)static\s+void\s+app_bms_snapshot_update_signal\s*\([^)]*\)\s*\{(?<body>.*?)\n\}')
if (-not $signalUpdateMatch.Success) {
    throw 'Could not find the signal snapshot update function.'
}
if ($signalUpdateMatch.Groups['body'].Value -match 'd_uart_|comm_active|load_removed|uart_rx_count|uart_tx_count|uart_tx_pending') {
    throw 'Snapshot input refresh must not maintain unused load/communication bookkeeping.'
}

Write-Output 'BMS signal snapshot minimal-field contract: PASS'
