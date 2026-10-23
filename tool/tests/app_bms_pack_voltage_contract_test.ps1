$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '../..')
$snapshotPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_snapshot.c'
$typesPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_types.h'
$snapshotSource = Get-Content -Raw $snapshotPath
$typesSource = Get-Content -Raw $typesPath

$sumMatch = [regex]::Match(
    $snapshotSource,
    '(?s)static\s+uint32_t\s+app_bms_snapshot_sum_cell_voltage\s*\([^)]*\)\s*\{(?<body>.*?)\n\}')
if (-not $sumMatch.Success) {
    throw 'Missing the logical-cell voltage summation helper.'
}

$sumBody = $sumMatch.Groups['body'].Value
foreach ($requiredExpression in @(
    'data_ready\s*==\s*0U',
    'cell_count\s*==\s*0U',
    'cell_count\s*>\s*APP_BMS_CELL_COUNT',
    'valid_cells\s*!=\s*ptSnapshot->cell_count',
    '\w+\s*\+=\s*ptSnapshot->cell_mv\[u8CellIndex\]')) {
    if ($sumBody -notmatch $requiredExpression) {
        throw "Pack voltage summation is missing required guard/operation: $requiredExpression"
    }
}

if ($typesSource -notmatch 'uint32_t\s+bat_mv;') {
    throw 'Snapshot must expose bat_mv for the logical-cell voltage sum.'
}

if ($snapshotSource -match 'gt_hsi_afe\.vpack') {
    throw 'BMS snapshot pack voltage must not use the AFE vpack field.'
}

$updateMatch = [regex]::Match(
    $snapshotSource,
    '(?s)void\s+app_bms_snapshot_update\s*\(void\)\s*\{(?<body>.*?)\n\}')
if (-not $updateMatch.Success) {
    throw 'Could not find app_bms_snapshot_update().' 
}

$updateBody = $updateMatch.Groups['body'].Value
$cellsUpdateIndex = $updateBody.IndexOf('app_bms_snapshot_update_cells(&next_snapshot)')
$sumIndex = $updateBody.IndexOf('app_bms_snapshot_sum_cell_voltage(&next_snapshot)')
if ($cellsUpdateIndex -lt 0 -or $sumIndex -le $cellsUpdateIndex) {
    throw 'Pack voltage must be calculated after the logical cell array is refreshed.'
}
if ($updateBody -notmatch 'next_snapshot\.bat_mv\s*=\s*app_bms_snapshot_sum_cell_voltage\(&next_snapshot\)\s*;') {
    throw 'Snapshot bat_mv must come from the logical cell voltage summation.'
}

Write-Output 'BMS pack voltage logical-cell summation contract: PASS'
