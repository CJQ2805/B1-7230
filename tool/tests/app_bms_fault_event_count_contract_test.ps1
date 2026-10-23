$ErrorActionPreference = 'Stop'

$projectRoot = Resolve-Path (Join-Path $PSScriptRoot '../..')
$faultHeaderPath = Join-Path $projectRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_fault.h'
$faultSourcePath = Join-Path $projectRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_fault.c'
$saveTaskPath = Join-Path $projectRoot 'app/N32G45x_KEIL/User_Src/Task/save_log_task.c'
$faultHeader = Get-Content -Raw $faultHeaderPath
$faultSource = Get-Content -Raw $faultSourcePath
$saveTask = Get-Content -Raw $saveTaskPath

foreach ($field in @('u32overcharge_count', 'u32overdischarge_count')) {
    if ($faultHeader -notmatch "uint32_t\s+$field\s*;") {
        throw "Global fault state must expose '$field' as a 32-bit counter."
    }
}

$recordMatch = [regex]::Match(
    $faultSource,
    '(?s)#pragma\s+pack\(push,\s*1\).*?typedef\s+struct\s*\{(?<fields>.*?)\}\s*bms_fault_pf_shutdown_record_t\s*;')
if (-not $recordMatch.Success -or
    $recordMatch.Groups['fields'].Value -notmatch 'u32overcharge_count' -or
    $recordMatch.Groups['fields'].Value -notmatch 'u32overdischarge_count') {
    throw 'PF flash record must persist both voltage event counters alongside the PF mask.'
}

foreach ($field in @('u32overcharge_count', 'u32overdischarge_count')) {
    if ($faultSource -notmatch "memcpy\([^;]*$field" -or
        $faultSource -notmatch "gt_app_bms_fault\.$field\s*=") {
        throw "PF save/load must serialize and restore '$field'."
    }
}
if ($faultSource -notmatch 'UINT32_MAX|0xFFFFFFFFUL|0xFFFFFFFFU') {
    throw 'Legacy PF records must interpret erased counter bytes as zero.'
}

$eventUpdateMatch = [regex]::Match(
    $faultSource,
    '(?s)static\s+void\s+bms_fault_update_voltage_event_counts\s*\(void\)\s*\{(?<body>.*?)\n\}')
if (-not $eventUpdateMatch.Success) {
    throw 'Fault task must detect new overcharge and overdischarge protection events.'
}
$eventBody = $eventUpdateMatch.Groups['body'].Value
$aggregationRequirements = @{
    bms_fault_is_overcharge_active = @('b1cell_pro_cov', 'b1cell_pro_cov2', 'b1pack_pro_pov')
    bms_fault_is_overdischarge_active = @('b1cell_pro_cuv1', 'b1cell_pro_cuv2', 'b1cell_deep_cuv', 'b1pack_pro_cuv')
}
foreach ($helper in $aggregationRequirements.Keys) {
    $helperMatch = [regex]::Match(
        $faultSource,
        "(?s)static\s+uint8_t\s+$helper\s*\([^)]*\)\s*\{(?<body>.*?)\n\}")
    if (-not $helperMatch.Success) {
        throw "Voltage event aggregation helper '$helper' is missing."
    }
    foreach ($flag in $aggregationRequirements[$helper]) {
        if ($helperMatch.Groups['body'].Value -notmatch "\b$flag\b") {
            throw "Voltage event aggregation helper '$helper' is missing protection flag '$flag'."
        }
    }
}
if ($eventBody -notmatch '(?s)current_overcharge_active\s*!=\s*0U.*previous_overcharge_active\s*==\s*0U' -or
    $eventBody -notmatch '(?s)current_overdischarge_active\s*!=\s*0U.*previous_overdischarge_active\s*==\s*0U' -or
    $eventBody -notmatch 'u32overcharge_count\s*\+\+' -or
    $eventBody -notmatch 'u32overdischarge_count\s*\+\+') {
    throw 'Counters must increment only on a false-to-true aggregate protection transition.'
}
if ($eventBody -notmatch 's_bms_pf_record_dirty\s*=\s*1U') {
    throw 'A new voltage event must schedule low-priority persistence.'
}
if ($faultSource -notmatch 'u32overcharge_count\s*<\s*0xFFFFFFFEUL' -or
    $faultSource -notmatch 'u32overdischarge_count\s*<\s*0xFFFFFFFEUL') {
    throw 'Counters must saturate below the erased-Flash sentinel value.'
}
if ($faultSource -notmatch 'bms_fault_update_voltage_event_counts\s*\(\s*\)\s*;\s*app_bms_fault_merge\s*\(\s*\)') {
    throw 'Voltage events must be counted after final protection aggregation and before fault snapshot merge.'
}

$loadMatch = [regex]::Match(
    $faultSource,
    '(?s)void\s+app_bms_fault_load_shutdown_pf\s*\(void\)\s*\{(?<body>.*?)\n\}')
if (-not $loadMatch.Success -or
    $loadMatch.Groups['body'].Value -notmatch 'bms_fault_restore_pf_mask' -or
    $loadMatch.Groups['body'].Value -notmatch 'u32overcharge_count' -or
    $loadMatch.Groups['body'].Value -notmatch 'u32overdischarge_count' -or
    $loadMatch.Groups['body'].Value -notmatch '0xFFFFFFFFUL' -or
    $loadMatch.Groups['body'].Value -notmatch 's_bms_overcharge_event_previous_active' -or
    $loadMatch.Groups['body'].Value -notmatch 's_bms_overdischarge_event_previous_active') {
    throw 'Startup PF loading must restore both counters and seed the active-event baseline.'
}

$storageTaskMatch = [regex]::Match(
    $faultSource,
    '(?s)void\s+app_bms_fault_storage_task\s*\(void\)\s*\{(?<body>.*?)\n\}')
if (-not $storageTaskMatch.Success -or
    $storageTaskMatch.Groups['body'].Value -notmatch 's_bms_pf_record_dirty' -or
    $storageTaskMatch.Groups['body'].Value -notmatch 'app_bms_fault_save_shutdown_pf\s*\(') {
    throw 'Pending PF counter changes must be saved by the background storage entry point.'
}
if ($saveTask -notmatch '(?s)void\s+SaveLog_Task.*?app_bms_fault_storage_task\s*\(\s*\)') {
    throw 'SaveLog_Task must service pending PF counter persistence.'
}

$clearMatch = [regex]::Match(
    $faultSource,
    '(?s)uint8_t\s+app_bms_fault_clear_shutdown_pf\s*\(void\)\s*\{(?<body>.*?)\n\}')
if (-not $clearMatch.Success -or
    $clearMatch.Groups['body'].Value -notmatch 'u32overcharge_count\s*=\s*0U' -or
    $clearMatch.Groups['body'].Value -notmatch 'u32overdischarge_count\s*=\s*0U' -or
    $clearMatch.Groups['body'].Value -notmatch 's_bms_pf_record_dirty\s*=\s*0U' -or
    $clearMatch.Groups['body'].Value -notmatch 's_bms_overcharge_event_previous_active\s*=\s*bms_fault_is_overcharge_active' -or
    $clearMatch.Groups['body'].Value -notmatch 's_bms_overdischarge_event_previous_active\s*=\s*bms_fault_is_overdischarge_active') {
    throw 'Successful PF clear must also clear both runtime counters and pending persistence.'
}

Write-Output 'BMS overcharge/overdischarge event counter contract: PASS'
