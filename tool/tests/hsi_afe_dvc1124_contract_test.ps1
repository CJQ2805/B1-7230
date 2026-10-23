$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppRoot = Join-Path $ProjectRoot 'app/N32G45x_KEIL'
$HsiRoot = Join-Path $AppRoot 'User_Src/HSI_afe'
$CanPath = Join-Path $AppRoot 'User_Src/Protocol/Comm/ducar_can_protocol.c'
$CanHeaderPath = Join-Path $AppRoot 'User_Src/Protocol/Comm/ducar_can_protocol.h'
$UartPath = Join-Path $AppRoot 'User_Src/Protocol/Comm/ducar_uart_protocol.c'
$DriverPath = Join-Path $HsiRoot 'hsi_afe_dvc1124.c'
$DriverHeaderPath = Join-Path $HsiRoot 'hsi_afe_dvc1124.h'
$OldDriverPath = Join-Path $HsiRoot 'hsi_afe_dvc1117.c'
$OldDriverHeaderPath = Join-Path $HsiRoot 'hsi_afe_dvc1117.h'
$AfeHeaderPath = Join-Path $HsiRoot 'hsi_afe.h'
$SnapshotPath = Join-Path $AppRoot 'User_Src/App/app_bms_snapshot.c'
$BalanceManagePath = Join-Path $AppRoot 'User_Src/App/app_balance_manage.c'

$script:ContractResults = [System.Collections.Generic.List[object]]::new()

function Add-ContractResult
{
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    $script:ContractResults.Add([pscustomobject]@{
        Name = $Name
        Passed = $Passed
        Detail = $Detail
    })
}

function Get-TextOrEmpty
{
    param([string]$Path)

    if (Test-Path -LiteralPath $Path)
    {
        return Get-Content -Raw -LiteralPath $Path
    }

    return ''
}

function Get-CFunctionBody
{
    param(
        [string]$SourceText,
        [string]$FunctionName
    )

    $FunctionNameIndex = $SourceText.IndexOf($FunctionName, [StringComparison]::Ordinal)
    if ($FunctionNameIndex -lt 0)
    {
        return ''
    }

    $OpenBraceIndex = $SourceText.IndexOf('{', $FunctionNameIndex)
    if ($OpenBraceIndex -lt 0)
    {
        return ''
    }

    $BraceDepth = 0
    for ($Index = $OpenBraceIndex; $Index -lt $SourceText.Length; $Index++)
    {
        if ($SourceText[$Index] -eq '{')
        {
            $BraceDepth++
        }
        elseif ($SourceText[$Index] -eq '}')
        {
            $BraceDepth--
            if ($BraceDepth -eq 0)
            {
                return $SourceText.Substring($OpenBraceIndex, $Index - $OpenBraceIndex + 1)
            }
        }
    }

    return ''
}

$ParamPath = Join-Path $AppRoot 'User_Src/App/app_param.h'
$ParamValidatePath = Join-Path $AppRoot 'User_Src/App/app_param_validate.h'
$ParamText = Get-TextOrEmpty $ParamPath
$ParamValidateText = Get-TextOrEmpty $ParamValidatePath
$AfeServicePath = Join-Path $HsiRoot 'hsi_afe.c'
$AfeServiceText = Get-TextOrEmpty $AfeServicePath
$AfeHeaderText = Get-TextOrEmpty $AfeHeaderPath
$SnapshotText = Get-TextOrEmpty $SnapshotPath
$BalanceManageText = Get-TextOrEmpty $BalanceManagePath
$DriverText = Get-TextOrEmpty $DriverPath
$CanText = Get-TextOrEmpty $CanPath
$CanHeaderText = Get-TextOrEmpty $CanHeaderPath
$UartText = Get-TextOrEmpty $UartPath

$AppCellCountMatch = [regex]::Match($ParamText, '(?m)^\s*#define\s+APP_BMS_MAX_CELL_NUM\s+(\d+)U\b')
$AppCellCount = 0
$AppCellCountIsValid = $false
if ($AppCellCountMatch.Success)
{
    $AppCellCount = [int]$AppCellCountMatch.Groups[1].Value
    $AppCellCountIsValid = ($AppCellCount -ge 1) -and ($AppCellCount -le 24)
}
Add-ContractResult 'APP maximum cell count is within DVC1124 range' $AppCellCountIsValid 'APP_BMS_MAX_CELL_NUM must be a compile-time value from 1 through 24.'
$AppCellCountHasDvc1124RangeGuard = ($ParamValidateText -match '(?m)^\s*#if\s*\(APP_BMS_MAX_CELL_NUM\s*<\s*1U\)\s*\|\|\s*\(APP_BMS_MAX_CELL_NUM\s*>\s*24U\)\s*$') -and
    ($ParamValidateText -match '(?m)^\s*#error\s+"APP_BMS_MAX_CELL_NUM must be between 1 and 24 for DVC1124"\s*$')
Add-ContractResult 'APP cell count has a compile-time DVC1124 range guard' $AppCellCountHasDvc1124RangeGuard 'Invalid cell counts must fail compilation instead of creating unsafe arrays or shifts.'

$NewDriverFilesExist = (Test-Path -LiteralPath $DriverPath) -and (Test-Path -LiteralPath $DriverHeaderPath)
$OldDriverFilesAbsent = (-not (Test-Path -LiteralPath $OldDriverPath)) -and (-not (Test-Path -LiteralPath $OldDriverHeaderPath))
Add-ContractResult 'DVC1124 driver files replace DVC1117 files' ($NewDriverFilesExist -and $OldDriverFilesAbsent) 'The HSI directory must contain hsi_afe_dvc1124.c/.h and no hsi_afe_dvc1117.c/.h.'

$LegacyReferences = @()
Get-ChildItem -LiteralPath $AppRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.h', '.uvprojx') } |
    ForEach-Object {
        if (Select-String -LiteralPath $_.FullName -Pattern 'hsi_afe_dvc1117_' -Quiet)
        {
            $LegacyReferences += $_.FullName.Substring($AppRoot.Length + 1)
        }
    }
$LegacyReferenceDetail = 'No legacy HSI symbol references found.'
if ($LegacyReferences.Count -gt 0)
{
    $LegacyReferenceDetail = 'Legacy references: ' + ($LegacyReferences -join ', ')
}
Add-ContractResult 'No DVC1117-specific HSI symbols remain' ($LegacyReferences.Count -eq 0) $LegacyReferenceDetail

$AfeInstanceSeparatesLogicalAndPhysicalCounts =
    ($AfeServiceText -match '(?s)\.cells\s*=\s*APP_BMS_CELL_COUNT\b') -and
    ($AfeServiceText -match '(?s)\.cell_scan_count\s*=\s*APP_BMS_AFE_SCAN_CELL_COUNT\b') -and
    ($AfeServiceText -match 'HSI_AFE_TASK_CURRENT\s*=\s*APP_BMS_AFE_SCAN_CELL_COUNT') -and
    ($AfeServiceText -match 'afe->cell_scan_count')
Add-ContractResult 'AFE separates logical series count from physical scan count' $AfeInstanceSeparatesLogicalAndPhysicalCounts 'The business cell count must not truncate the 24-channel AFE scan.'

$SnapshotUsesTopologyMappedVoltages =
    ($SnapshotText -match 'app_bms_topology_get_cell_voltage_mv\s*\(\s*gt_hsi_afe\.vol') -and
    ($SnapshotText -notmatch 'gt_hsi_afe\.vol\s*\[\s*u8CellIndex\s*\]')
Add-ContractResult 'BMS snapshot normalizes physical AFE channels to logical cells' $SnapshotUsesTopologyMappedVoltages 'All business consumers must receive the mapped, contiguous logical cell array.'

$BalanceUsesReverseTopologyMap = $BalanceManageText -match 'app_bms_topology_balance_to_afe\s*\(\s*u32BalanceRequestMask'
Add-ContractResult 'Balance requests map logical top cell back to its physical AFE channel' $BalanceUsesReverseTopologyMap 'The logical last-cell balance bit must target the physical top-cell channel.'

$AfeBalanceStatePreservesAllConfiguredBits = $AfeHeaderText -match '(?m)^\s*uint32_t\s+balstate\s*;'
Add-ContractResult 'AFE balance state preserves all configured cell bits' $AfeBalanceStatePreservesAllConfiguredBits 'hsi_afe_t.balstate must be uint32_t to hold the maximum DVC1124 configuration of 24 cells.'
$AfeBalanceStateAssignmentPreserves20BitMask = $AfeServiceText -notmatch 'balstate\s*=\s*\(uint16_t\)\s*request'
Add-ContractResult 'AFE balance state assignment does not narrow the mask' $AfeBalanceStateAssignmentPreserves20BitMask 'The 32-bit request must not be cast to uint16_t before storing it.'

$AfeBalanceMasksUseConfiguredCellCount =
    ($AfeHeaderText -match '(?m)^\s*#define\s+HSI_AFE_BALANCE_MASK_ALL\s+\(\(1UL\s*<<\s*APP_BMS_MAX_CELL_NUM\)\s*-\s*1UL\)') -and
    ($AfeHeaderText -match '(?m)^\s*BALANCE_MASK_L\s*=\s*\(HSI_AFE_BALANCE_MASK_ALL\s*&\s*0x0000FFUL\)\s*,') -and
    ($AfeHeaderText -match '(?m)^\s*BALANCE_MASK_M\s*=\s*\(HSI_AFE_BALANCE_MASK_ALL\s*&\s*0x00FF00UL\)\s*,') -and
    ($AfeHeaderText -match '(?m)^\s*BALANCE_MASK_H\s*=\s*\(HSI_AFE_BALANCE_MASK_ALL\s*&\s*0xFF0000UL\)\s*,') -and
    ($AfeHeaderText -match '(?m)^\s*BALANCE_MASK_All\s*=\s*HSI_AFE_BALANCE_MASK_ALL\s*$')
Add-ContractResult 'AFE balance masks cover the physical channel capacity' $AfeBalanceMasksUseConfiguredCellCount 'The low, middle, high, and all-channel masks must cover APP_BMS_MAX_CELL_NUM physical AFE bits.'

$CellAddressBody = Get-CFunctionBody $DriverText 'hsi_afe_dvc1124_cell_reg_addr'
$HasDvc1124CellAddressFormula = ($CellAddressBody -match '0x1D\s*\+\s*\(\(uint16_t\)u8CellIndex\s*\*\s*2U\)')
Add-ContractResult 'Cell index maps from register 0x1D in two-byte steps' $HasDvc1124CellAddressFormula 'Cell indexes 0, 15, 16, and 19 must map to 0x1D, 0x3B, 0x3D, and 0x43.'

$BalanceBody = Get-CFunctionBody $DriverText 'hsi_afe_dvc1124_pack_balance'
$BalanceMaskIsBounded = $BalanceBody -match 'u32CellMask\s*&=\s*hsi_afe_dvc1124_cell_mask\s*\(\s*AFE_MAX_CELL_CNT\s*\)'
$BalanceBytesAreOrdered = ($BalanceBody -match 'pau8CbRegisters\s*\[\s*0\s*\]\s*=\s*\(uint8_t\)\s*\(\s*\(\s*u32CellMask\s*>>\s*16U\s*\)\s*&\s*0xFFU\s*\)') -and
    ($BalanceBody -match 'pau8CbRegisters\s*\[\s*1\s*\]\s*=\s*\(uint8_t\)\s*\(\s*\(\s*u32CellMask\s*>>\s*8U\s*\)\s*&\s*0xFFU\s*\)') -and
    ($BalanceBody -match 'pau8CbRegisters\s*\[\s*2\s*\]\s*=\s*\(uint8_t\)\s*\(\s*u32CellMask\s*&\s*0xFFU\s*\)')
Add-ContractResult 'DVC1124 balance packer writes three CB bytes in register order' ($BalanceMaskIsBounded -and $BalanceBytesAreOrdered) 'CB[0] receives mask bits 16-23, CB[1] bits 8-15, and CB[2] bits 0-7, after clamping to the configured cell count.'

$CanKeepsSixFourCellFrames = ($CanHeaderText -match 'DUCAR_CAN_PGN_F20F') -and
    ($CanHeaderText -match 'DUCAR_CAN_PGN_F214') -and
    ($CanText -match 'ducar_can_cell_voltage_mv\s*\(\s*&tBmsSnapshot\s*,\s*19U\s*\)') -and
    ($CanText -match 'ducar_can_cell_voltage_mv\s*\(\s*&tBmsSnapshot\s*,\s*23U\s*\)')
$CanRejectsCellsBeyondSnapshot = ($CanText -match 'u8CellIndex\s*>=\s*ptBmsSnapshot->cell_count') -and
    ($CanText -match 'u8CellIndex\s*>=\s*APP_BMS_CELL_COUNT') -and
    ($CanText -match 'return\s+DUCAR_CAN_INVALID_U16\s*;')
$UartKeepsThreeEightCellGroups = ($UartText -match 'ducar_bms_reply_cell_group\s*\(\s*frame->cmd\s*,\s*0U\s*\)') -and
    ($UartText -match 'ducar_bms_reply_cell_group\s*\(\s*frame->cmd\s*,\s*8U\s*\)') -and
    ($UartText -match 'ducar_bms_reply_cell_group\s*\(\s*frame->cmd\s*,\s*16U\s*\)') -and
    ($UartText -match 'u8CellIndex\s*<\s*u8CellCount')
Add-ContractResult 'CAN/UART preserve frame layout and bound reads to logical snapshot count' ($CanKeepsSixFourCellFrames -and $CanRejectsCellsBeyondSnapshot -and $UartKeepsThreeEightCellGroups) 'CAN F20F–F214 and UART 3×8 frame layouts remain unchanged; array reads are limited to the configured logical cell count.'

$DesignVoltageUsesLogicalSeriesCount = $UartText -match 'APP_BMS_CELL_COUNT\s*\*\s*DUCAR_BMS_DESIGN_CELL_MV'
Add-ContractResult 'UART design voltage uses configured logical series count' $DesignVoltageUsesLogicalSeriesCount 'Pack design voltage must be calculated from the 20S/21S logical configuration, not the 24-channel AFE capacity.'

$FailureCount = @($script:ContractResults | Where-Object { -not $_.Passed }).Count
foreach ($Result in $script:ContractResults)
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
    throw "$FailureCount DVC1124 AFE source-contract assertion(s) failed."
}
