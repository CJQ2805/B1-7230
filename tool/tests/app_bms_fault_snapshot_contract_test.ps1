$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppRoot = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App'
$BmsTypesPath = Join-Path $AppRoot 'app_bms_types.h'
$SnapshotPath = Join-Path $AppRoot 'app_bms_snapshot.c'
$CanPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c'
$UartPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_uart_protocol.c'

foreach ($RequiredPath in @($BmsTypesPath, $SnapshotPath, $CanPath, $UartPath))
{
    if (-not (Test-Path -LiteralPath $RequiredPath))
    {
        throw "Required fault snapshot source is missing: $RequiredPath"
    }
}

$BmsTypesText = Get-Content -Raw -LiteralPath $BmsTypesPath
$SnapshotText = Get-Content -Raw -LiteralPath $SnapshotPath
$CanText = Get-Content -Raw -LiteralPath $CanPath
$UartText = Get-Content -Raw -LiteralPath $UartPath

$SnapshotStoresNativeFaultUnion =
    ($BmsTypesText -match '#include\s+"App/app_bms_fault\.h"') -and
    ($BmsTypesText -match 't_bms_fault_flag\s+detail\s*;') -and
    ($BmsTypesText -match 'uint8_t\s+charge_pro\s*;') -and
    ($BmsTypesText -match 'uint8_t\s+discharge_pro\s*;')
if (-not $SnapshotStoresNativeFaultUnion)
{
    throw 'Snapshot fault image must store the native fault union and retain protection summaries.'
}

$SnapshotUsesCoherentFaultCopy =
    ($SnapshotText -match 'struct\s+_app_bms_fault_t\s+tFaultSnapshot') -and
    ($SnapshotText -match 'app_bms_fault_snapshot_read\s*\(\s*&tFaultSnapshot\s*\)') -and
    ($SnapshotText -match 'image->detail\s*=\s*\*fault\s*;') -and
    ($SnapshotText -match 'snapshot->fault\.software\s*,\s*&tFaultSnapshot\.tbms_sw_fault_flag') -and
    ($SnapshotText -match 'snapshot->fault\.active\s*,\s*&tFaultSnapshot\.tbms_fault_flag') -and
    ($SnapshotText -match 'snapshot->fault\.saved\s*,\s*&tFaultSnapshot\.tsave_bms_fault_flag')
if (-not $SnapshotUsesCoherentFaultCopy)
{
    throw 'Snapshot must copy software, active, and previous fault unions from one coherent fault-module snapshot.'
}

$CanKeepsRawUnionWords = $CanText -match 'fault\.active\.detail\.ucharge_fault_A\.u16fault_A'
$UartCopiesNativeFaultUnion = $UartText -match '\*fault\s*=\s*snapshot->fault\.active\.detail\s*;'
if (-not ($CanKeepsRawUnionWords -and $UartCopiesNativeFaultUnion))
{
    throw 'CAN must use raw 16-bit fault words and UART must copy the same native fault union from the snapshot.'
}

Write-Output 'PASS: Snapshot fault entries retain native named union bitfields and raw words.'
Write-Output 'PASS: Software/active/saved fault maps come from one coherent fault-module copy.'
Write-Output 'PASS: CAN reads raw 16-bit union words; UART copies the native fault union.'
