$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$InputHeaderPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_input_signals.h'
$InputSourcePath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_input_signals.c'
$SnapshotTypesPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_bms_types.h'
$SnapshotSourcePath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.c'
$CanProtocolPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c'
$LedPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_led.c'
$LowPowerPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\App\app_low_power_manage.c'
$Header = Get-Content -Raw -Encoding UTF8 -LiteralPath $InputHeaderPath
$Source = Get-Content -Raw -LiteralPath $InputSourcePath
$SnapshotTypes = Get-Content -Raw -LiteralPath $SnapshotTypesPath
$SnapshotSource = Get-Content -Raw -LiteralPath $SnapshotSourcePath
$CanProtocol = Get-Content -Raw -LiteralPath $CanProtocolPath
$Led = Get-Content -Raw -LiteralPath $LedPath
$LowPower = Get-Content -Raw -LiteralPath $LowPowerPath

if ($Header -notmatch '(?s)typedef\s+struct\s*\{(?<fields>.*?)\}\s*app_input_signals_t\s*;')
{
    throw 'Could not locate the public app_input_signals_t result structure.'
}
$PublicFields = $Matches['fields']

foreach ($Field in @('mcu_det_load', 'mcu_detd', 'mcu_detc', 'mcu_2nd_det', 'mcu_sw_present'))
{
    if ($PublicFields -notmatch "\b$Field\b")
    {
        throw "Public input results must include '$Field'."
    }
}

foreach ($InternalOnlyField in @('raw_level', 'stable_level', 'connected', 'removed', 'changed', 'pending_count', 'mcu_sw_level', 'mcu_sw_pressed', 'mcu_sw_debounce_count', 'mcu_sw_release_count', 'mcu_2nd_det_level'))
{
    if ($PublicFields -match "\b$InternalOnlyField\b")
    {
        throw "Internal sampling state '$InternalOnlyField' must not be exposed in app_input_signals_t."
    }
}

if ($Header -match '\bapp_input_debounce_t\b')
{
    throw 'The generic debounce implementation type must remain private to the C module.'
}

if ($Header -notmatch 'mcu_sw_present\s*;\s*/\*[^*]*\u677E\u5F00\u540E5\u79D2\u5185')
{
    throw 'The public MCU_SW activity result must document its 5-second release hold semantics.'
}

if ($CanProtocol -notmatch 'signal\.input\.mcu_sw_present' -or
    $Led -notmatch 'signal\.input\.mcu_sw_present' -or
    $LowPower -notmatch 'signal\.input\.mcu_sw_present')
{
    throw 'CAN reporting, LED state, and low-power gating must retain their existing MCU_SW activity result.'
}

foreach ($StateField in @('u8RawLevel', 'stable_connected', 'pending_count', 's_mcu_sw_pressed', 's_mcu_sw_debounce_count', 's_mcu_sw_release_count'))
{
    if ($Source -notmatch "\b$StateField\b")
    {
        throw "Debounce state '$StateField' must be handled internally by app_input_signals.c."
    }
}

if ($Source -match 'gt_app_input_signals\.mcu_sw_pressed')
{
    throw 'The current pressed state may be used internally but must not be published in app_input_signals_t.'
}

if ($Source -notmatch '#define\s+APP_INPUT_SIGNAL_DEBOUNCE_TIME_MS\s+500U' -or
    $Source -notmatch '#define\s+APP_INPUT_SIGNAL_MCU_SW_DEBOUNCE_TIME_MS\s+20U' -or
    $Source -notmatch '#define\s+APP_INPUT_SIGNAL_MCU_SW_PRESENT_HOLD_TIME_MS\s+5000U')
{
    throw 'The existing external-input and MCU_SW timing contract must remain unchanged.'
}

$SourceFiles = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src') -Recurse -File |
    Where-Object { $_.Extension -in @('.c', '.h') }
foreach ($File in $SourceFiles)
{
    $Text = Get-Content -Raw -LiteralPath $File.FullName
    if ($Text -match '\bDET_LD\b')
    {
        throw "Legacy input name 'DET_LD' is still referenced in $($File.FullName)."
    }

    if ($Text -match '\bmcu_det(?:_ld|d|c)\.(?:connected|removed|raw_level|stable_level|pending_count)\b')
    {
        throw "Legacy nested DET state is still referenced in $($File.FullName)."
    }
}

if ($Source -notmatch 'gt_app_input_signals\.mcu_2nd_det\s*=\s*\(board_io_mcu_2nd_det_level\(\)\s*==\s*0U\)\s*\?\s*1U\s*:\s*0U;')
{
    throw 'MCU_2ND_DET must publish the inverted GPIO level: low=active/present, high=inactive/absent.'
}

if ($SnapshotTypes -match '\bload_detect_level\b' -or $SnapshotSource -match '\bload_detect_level\b')
{
    throw 'The BMS snapshot must expose only the interpreted load-removed result, not the raw load-detect pin level.'
}

foreach ($RemovedSignalField in @('load_removed', 'comm_active', 'uart_rx_count', 'uart_tx_count', 'uart_tx_pending'))
{
    if ($SnapshotTypes -match "\b$RemovedSignalField\b" -or
        $SnapshotSource -match "snapshot->signal\.$RemovedSignalField\b")
    {
        throw "Unused snapshot signal field '$RemovedSignalField' must not be exposed or refreshed."
    }
}

Write-Output 'Input final-result structure contract: PASS'
