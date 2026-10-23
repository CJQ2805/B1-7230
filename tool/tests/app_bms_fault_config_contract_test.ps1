$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppRoot = Join-Path $ProjectRoot 'app/N32G45x_KEIL'
$AppSourceRoot = Join-Path $AppRoot 'User_Src'
$FaultConfigPath = Join-Path $AppSourceRoot 'App/app_bms_fault_config.h'
$FaultSourcePath = Join-Path $AppSourceRoot 'App/app_bms_fault.c'
$LegacyConfigPath = Join-Path $AppSourceRoot 'App/app_bms_config.h'
$KeilProjectPath = Join-Path $AppRoot 'MDK-ARM/N32G45x.uvprojx'

function Add-ContractResult
{
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    [pscustomobject]@{
        Name = $Name
        Passed = $Passed
        Detail = $Detail
    }
}

$ContractResults = [System.Collections.Generic.List[object]]::new()
$FaultConfigText = if (Test-Path -LiteralPath $FaultConfigPath) { Get-Content -Raw -LiteralPath $FaultConfigPath } else { '' }
$FaultSourceText = if (Test-Path -LiteralPath $FaultSourcePath) { Get-Content -Raw -LiteralPath $FaultSourcePath } else { '' }
$KeilProjectText = if (Test-Path -LiteralPath $KeilProjectPath) { Get-Content -Raw -LiteralPath $KeilProjectPath } else { '' }

$ContractResults.Add((Add-ContractResult 'Legacy BMS config header is removed' (-not (Test-Path -LiteralPath $LegacyConfigPath)) 'app_bms_config.h must not remain after configuration consolidation.'))
$ContractResults.Add((Add-ContractResult 'Fault config header exists' (Test-Path -LiteralPath $FaultConfigPath) 'app_bms_fault_config.h must remain as the single fault configuration entry.'))
$ContractResults.Add((Add-ContractResult 'Fault config does not include legacy header' ($FaultConfigText -notmatch 'app_bms_config\.h') 'The merged header must be self-contained and must not include app_bms_config.h.'))
$ContractResults.Add((Add-ContractResult 'Fault config owns protection switches' (($FaultConfigText -match '(?m)^\s*#define\s+BMS_FEATURE_FAULT_PROTECT_ENABLE\b') -and
        ($FaultConfigText -match '(?m)^\s*#define\s+BMS_PROTECT_AFE_FAULT_ENABLE\b') -and
        ($FaultConfigText -match '(?m)^\s*#define\s+BMS_PROTECT_NTC_FAULT_ENABLE\b') -and
        ($FaultConfigText -match '(?m)^\s*#define\s+BMS_LOAD_DETECT_ENABLE\b')) 'All BMS protection switches must be defined by app_bms_fault_config.h.'))
$SubSwitchNames = @(
    'BMS_PROTECT_AFE_FAULT_ENABLE',
    'BMS_PROTECT_NTC_FAULT_ENABLE',
    'BMS_PROTECT_CELL_OV_WARN_ENABLE',
    'BMS_PROTECT_CELL_OV_PRO_ENABLE',
    'BMS_PROTECT_CELL_OV2_PRO_ENABLE',
    'BMS_PROTECT_PACK_OV_WARN_ENABLE',
    'BMS_PROTECT_PACK_OV_PRO_ENABLE',
    'BMS_PROTECT_CELL_UV_WARN_ENABLE',
    'BMS_PROTECT_CELL_UV1_PRO_ENABLE',
    'BMS_PROTECT_CELL_UV2_PRO_ENABLE',
    'BMS_PROTECT_CELL_DEEP_UV_ENABLE',
    'BMS_PROTECT_PACK_UV_WARN_ENABLE',
    'BMS_PROTECT_PACK_UV_PRO_ENABLE',
    'BMS_PROTECT_CHG_OT_WARN_ENABLE',
    'BMS_PROTECT_CHG_OT_PRO_ENABLE',
    'BMS_PROTECT_CHG_UT_WARN_ENABLE',
    'BMS_PROTECT_CHG_UT_PRO_ENABLE',
    'BMS_PROTECT_DSG_OT_WARN_ENABLE',
    'BMS_PROTECT_DSG_OT_PRO_ENABLE',
    'BMS_PROTECT_DSG_UT_WARN_ENABLE',
    'BMS_PROTECT_DSG_UT_PRO_ENABLE',
    'BMS_PROTECT_MOS_OT_WARN_ENABLE',
    'BMS_PROTECT_MOS_OT_PRO_ENABLE',
    'BMS_PROTECT_CHG_OC_WARN_ENABLE',
    'BMS_PROTECT_CHG_OC_PRO_ENABLE',
    'BMS_PROTECT_CHG_OC_LOCK_ENABLE',
    'BMS_PROTECT_DSG_OC_WARN_ENABLE',
    'BMS_PROTECT_DSG_OC_PRO_ENABLE',
    'BMS_PROTECT_DSG_OC_LOCK_ENABLE',
    'BMS_PROTECT_SHORT_ENABLE',
    'BMS_LOAD_DETECT_ENABLE',
    'BMS_PROTECT_CELL_DIFF_WARN_ENABLE',
    'BMS_PROTECT_CELL_DIFF_PRO_ENABLE'
)
$DisabledBranchMatch = [regex]::Match($FaultConfigText, '(?s)#if\s*\(BMS_FEATURE_FAULT_PROTECT_ENABLE\s*==\s*0U\)(?<DisabledBranch>.*?)#else')
$SubSwitchesForcedOff = $DisabledBranchMatch.Success
if ($SubSwitchesForcedOff)
{
    foreach ($SubSwitchName in $SubSwitchNames)
    {
        if ($DisabledBranchMatch.Groups['DisabledBranch'].Value -notmatch ("(?m)^\s*#define\s+" + [regex]::Escape($SubSwitchName) + "\s+0U\b"))
        {
            $SubSwitchesForcedOff = $false
            break
        }
    }
}
$ContractResults.Add((Add-ContractResult 'Master protection switch disables all sub-switches' $SubSwitchesForcedOff 'When BMS_FEATURE_FAULT_PROTECT_ENABLE is 0U, every protection sub-switch must be forced to 0U.'))
$ContractResults.Add((Add-ContractResult 'Keil project has no legacy header entry' ($KeilProjectText -notmatch 'app_bms_config\.h') 'The deleted header must also be removed from the Keil project tree.'))

$FaultTaskMatch = [regex]::Match($FaultSourceText, '(?s)void\s+app_bms_fault_task\s*\([^)]*\)\s*\{(?<TaskBody>.*)\z')
$FaultTaskBody = if ($FaultTaskMatch.Success) { $FaultTaskMatch.Groups['TaskBody'].Value } else { '' }
$MasterSourceGated = ($FaultTaskBody -match '(?s)#if\s*\(BMS_FEATURE_FAULT_PROTECT_ENABLE\s*!=\s*0U\).*?bms_fault_update_afe_fault\s*\(\s*&data\s*\).*?bms_fault_update_general_faults\s*\(\s*&data\s*\).*?#else.*?memset\s*\(.*?tbms_sw_fault_flag.*?#endif')
$ContractResults.Add((Add-ContractResult 'Fault task follows master protection switch' $MasterSourceGated 'The .c task entry must compile out software threshold protection when BMS_FEATURE_FAULT_PROTECT_ENABLE is 0U.'))

$IdleOcdLockResetTimeConfigured = ($FaultConfigText -match '(?m)^\s*#define\s+OCD_LOCK_IDLE_RECOVER_TIME\s+TMR0_DELAY_MIN\(30U\)(?=\s|$)')
$ContractResults.Add((Add-ContractResult 'Discharge OC lock idle reset is configured for 30 minutes' $IdleOcdLockResetTimeConfigured 'The discharge overcurrent lock counter and lock bit must use the agreed 30-minute no-discharge interval.'))
$IdleOcdLockRecoveryMatch = [regex]::Match($FaultSourceText, '(?s)static\s+void\s+bms_fault_update_ocd_lock_idle_recovery\s*\([^)]*\)\s*\{(?<Body>.*?)\n\}\s*\n\s*void\s+app_bms_fault_task')
$IdleOcdLockRecoveryBody = if ($IdleOcdLockRecoveryMatch.Success) { $IdleOcdLockRecoveryMatch.Groups['Body'].Value } else { '' }
$IdleOcdLockRecoveryContract = ($IdleOcdLockRecoveryMatch.Success -and
    ($IdleOcdLockRecoveryBody -match 'data->data_ready\s*&&\s*\(data->discharging\s*==\s*false\)') -and
    ($IdleOcdLockRecoveryBody -match 'd_timer_delay_check\s*\(\s*true\s*,\s*&dly_ocd_lock_idle_recover\s*,\s*OCD_LOCK_IDLE_RECOVER_TIME\s*\)') -and
    ($IdleOcdLockRecoveryBody -match 's_bms_ocd_recover_count\s*=\s*0U') -and
    ($IdleOcdLockRecoveryBody -match 'b1pack_ocd_lock\s*=\s*0U') -and
    ($IdleOcdLockRecoveryBody -match 'd_timer_delay_check\s*\(\s*false\s*,\s*&dly_ocd_lock_idle_recover\s*,\s*OCD_LOCK_IDLE_RECOVER_TIME\s*\)') -and
    ($FaultTaskBody -match 'bms_fault_update_ocd_lock_idle_recovery\s*\(\s*&data\s*\)'))
$ContractResults.Add((Add-ContractResult 'No-discharge timeout clears OCD lock and counter' $IdleOcdLockRecoveryContract 'A valid no-discharge state must persist for 60 seconds; invalid data or discharge must restart timing.'))

$ChildSourceGatePatterns = @(
    '(?s)#if\s*\(BMS_PROTECT_AFE_FAULT_ENABLE\s*!=\s*0U\).*?bms_fault_update_afe_fault',
    '(?s)#if\s*\(BMS_PROTECT_CELL_OV_WARN_ENABLE\s*!=\s*0U\).*?b1cell_war_cov.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_CHG_OT_WARN_ENABLE\s*!=\s*0U\).*?b1pack_war_otc.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_CHG_OC_WARN_ENABLE\s*!=\s*0U\).*?b1pack_war_occ.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_DSG_OC_PRO_ENABLE\s*!=\s*0U\).*?b1pack_pro_ocd1.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_SHORT_ENABLE\s*!=\s*0U\).*?b1pack_shrot.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_NTC_FAULT_ENABLE\s*!=\s*0U\).*?b1NTC_fault.*?#endif',
    '(?s)#if\s*\(BMS_PROTECT_CELL_DIFF_WARN_ENABLE\s*!=\s*0U\).*?b1cell_voltage_diff_waring.*?#endif'
)
$DisabledCodeIsCompiledOut = $true
foreach ($ChildSourceGatePattern in $ChildSourceGatePatterns)
{
    if ($FaultSourceText -notmatch $ChildSourceGatePattern)
    {
        $DisabledCodeIsCompiledOut = $false
        break
    }
}
$ContractResults.Add((Add-ContractResult 'Disabled fault branches are compiled out in C source' $DisabledCodeIsCompiledOut 'Each representative protection branch must be enclosed by its matching child configuration switch.'))

$DelayDeclarationMacroDefined = ($FaultSourceText -match '(?m)^\s*#define\s+BMS_DECLARE_DELAY_PAIR\s*\(prefix\)')
$ContractResults.Add((Add-ContractResult 'Fault delay pairs have one declaration macro' $DelayDeclarationMacroDefined 'Each set/recover timer pair must be declared through one common macro.'))

$ExpectedDelayMacroCalls = @(
    'BMS_DECLARE_DELAY_PAIR\(dly_cov_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cov_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cov2_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cuv_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cuv1_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cuv2_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_pov_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_pov_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_puv_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_puv_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_otc_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_otc_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_utc_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_utc_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_otd_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_otd_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_utd_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_utd_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_mos_ot_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_mos_ot_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_occ_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_occ1_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_short\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cell_diff_war\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_cell_diff_pro\)',
    'BMS_DECLARE_DELAY_PAIR\(dly_ntc_fault\)'
)
$DelayMacroCallsArePresent = $true
foreach ($ExpectedDelayMacroCall in $ExpectedDelayMacroCalls)
{
    if ($FaultSourceText -notmatch $ExpectedDelayMacroCall)
    {
        $DelayMacroCallsArePresent = $false
        break
    }
}
$ContractResults.Add((Add-ContractResult 'Protection delay pairs are declared by feature group' $DelayMacroCallsArePresent 'Every protection delay pair must be visible as one named macro call in its grouped state declaration.'))

$CombinedVoltageGuard = $FaultSourceText -match '(?m)^\s*#if[^\r\n]*BMS_PROTECT_(CELL|PACK)_(OV|UV)_[A-Z0-9_]+_ENABLE[^\r\n]*(\|\||&&)[^\r\n]*BMS_PROTECT_(CELL|PACK)_(OV|UV)_[A-Z0-9_]+_ENABLE'
$ContractResults.Add((Add-ContractResult 'Voltage protection child switches remain independent' (-not $CombinedVoltageGuard) 'OV/UV warning and protection switches must not be merged through a combined preprocessor guard.'))

$LegacyReferences = @()
Get-ChildItem -LiteralPath $AppSourceRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.h', '.uvprojx') } |
    ForEach-Object {
        if (Select-String -LiteralPath $_.FullName -Pattern 'app_bms_config\.h' -Quiet)
        {
            $LegacyReferences += $_.FullName.Substring($ProjectRoot.Length + 1)
        }
    }
$ContractResults.Add((Add-ContractResult 'Application source has no legacy header references' ($LegacyReferences.Count -eq 0) ('Legacy references: ' + ($LegacyReferences -join ', '))))

$FailureCount = @($ContractResults | Where-Object { -not $_.Passed }).Count
foreach ($Result in $ContractResults)
{
    if ($Result.Passed)
    {
        Write-Output ('PASS: ' + $Result.Name)
    }
    else
    {
        Write-Output ('FAIL: ' + $Result.Name + ' - ' + $Result.Detail)
    }
}

if ($FailureCount -gt 0)
{
    throw "$FailureCount BMS fault configuration consolidation assertion(s) failed."
}
