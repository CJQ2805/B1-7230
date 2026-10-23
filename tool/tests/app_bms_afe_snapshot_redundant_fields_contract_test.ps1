$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$TypesPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_bms_types.h'
$SnapshotPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.c'
$Types = Get-Content -Raw -LiteralPath $TypesPath
$Snapshot = Get-Content -Raw -LiteralPath $SnapshotPath

$AfeFaultMatch = [regex]::Match(
    $Types,
    '(?s)typedef\s+struct\s*\{(?<fields>.*?)\}\s*app_bms_afe_fault_t\s*;')
if (-not $AfeFaultMatch.Success) {
    throw 'Could not locate app_bms_afe_fault_t.'
}
if ($AfeFaultMatch.Groups['fields'].Value -match '\braw\s*\[') {
    throw 'The snapshot must not duplicate the AFE raw status bytes beside their decoded fault fields.'
}

if ($Types -match '\bafe_ntc_fault\b') {
    throw 'AFE NTC fault must be represented by ntc.fault_mask, not a duplicate snapshot field.'
}

if ($Snapshot -match 'afe_fault\.raw\s*\[') {
    throw 'Snapshot refresh must not copy duplicate raw AFE status bytes.'
}

$NtcUpdateMatch = [regex]::Match(
    $Snapshot,
    '(?s)static\s+void\s+app_bms_snapshot_update_temps\s*\([^)]*\)\s*\{(?<body>.*?)\n\}')
if (-not $NtcUpdateMatch.Success -or
    $NtcUpdateMatch.Groups['body'].Value -notmatch 'ntc_abnormal_count\s*=\s*\(\(ptSnapshot->ntc\.fault_mask\s*&\s*APP_BMS_NTC_FAULT_AFE\)\s*!=\s*0U\)\s*\?\s*1U\s*:\s*0U;') {
    throw 'NTC abnormal count must include one AFE channel directly from ntc.fault_mask.'
}

Write-Output 'AFE snapshot redundant-field contract: PASS'
