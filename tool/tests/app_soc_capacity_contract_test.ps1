$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppSocPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\SOX\app_soc.h'
$BmsTaskPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\task\bms_task.c'

$AppSocText = Get-Content -Raw -LiteralPath $AppSocPath
$BmsTaskText = Get-Content -Raw -LiteralPath $BmsTaskPath

if ($AppSocText -notmatch '#include\s+"App/app_param\.h"')
{
    throw 'SOC configuration must include the shared application parameter definitions.'
}

if ($AppSocText -notmatch '#define\s+SOC_DESIGN_CAP\s+APP_PARAM_BAT_CAPACITY\b')
{
    throw 'SOC design capacity must use APP_PARAM_BAT_CAPACITY as its single source of truth.'
}

if ($BmsTaskText -notmatch '(?s)restored_soc\.s32battery_full_cap\s*<\s*MIN_FULLCHG_CAP.*?restored_soc\.s32battery_full_cap\s*>\s*MAX_FULLCHG_CAP')
{
    throw 'SOC Flash restore must reject capacities outside the configured FCC range.'
}

Write-Output 'SOC design capacity and persisted-capacity contract: PASS'
