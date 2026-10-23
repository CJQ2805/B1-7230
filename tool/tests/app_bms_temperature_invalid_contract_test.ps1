$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '../..')
$typesPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_types.h'
$snapshotPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/App/app_bms_snapshot.c'
$canProtocolPath = Join-Path $repoRoot 'app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c'
$typesSource = Get-Content -Raw $typesPath
$snapshotSource = Get-Content -Raw $snapshotPath
$canProtocolSource = Get-Content -Raw $canProtocolPath

function Assert-SourcePattern([string]$Source, [string]$Pattern, [string]$Message) {
    if ($Source -notmatch $Pattern) {
        throw $Message
    }
}

Assert-SourcePattern $typesSource '#define\s+APP_BMS_TEMP_INVALID_VALUE\s+\(\(int16_t\)0x7FFF\)' `
    'Temperature invalid sentinel must be explicitly defined as int16_t 0x7FFF.'
Assert-SourcePattern $snapshotSource 'ptSnapshot->temp_c\[u8TemperatureIndex\]\s*=\s*APP_BMS_TEMP_INVALID_VALUE\s*;' `
    'Invalid Celsius channels must not default to 0 C.'
Assert-SourcePattern $snapshotSource 'ptSnapshot->temp_0p1c\[u8TemperatureIndex\]\s*=\s*APP_BMS_TEMP_INVALID_VALUE\s*;' `
    'Invalid 0.1 C channels must use the declared invalid sentinel.'
Assert-SourcePattern $snapshotSource 'ptSnapshot->max_temp_c\s*=\s*APP_BMS_TEMP_INVALID_VALUE\s*;' `
    'Maximum temperature must be explicitly invalid before any valid sample is found.'
Assert-SourcePattern $snapshotSource 'ptSnapshot->min_temp_c\s*=\s*APP_BMS_TEMP_INVALID_VALUE\s*;' `
    'Minimum temperature must be explicitly invalid before any valid sample is found.'
Assert-SourcePattern $snapshotSource 'app_bms_snapshot_init_temp_values\(&gt_app_bms_snapshot\);' `
    'The startup snapshot must initialize temperature values as invalid before the first sample update.'
Assert-SourcePattern $snapshotSource 'app_bms_snapshot_init_temp_values\(ptSnapshot\);' `
    'Each refreshed snapshot must use the same invalid-temperature initialization.'
Assert-SourcePattern $canProtocolSource '#define\s+DUCAR_CAN_INVALID_S16\s+\(\(int16_t\)-400\)' `
    'Invalid CAN temperatures must be sent as -40.0 C at 0.1 C resolution.'
Assert-SourcePattern $canProtocolSource 'return\s+ptBmsSnapshot->temp_0p1c\[u8TemperatureIndex\];' `
    'Valid temperatures, including 0 C, must pass through without replacement.'
Assert-SourcePattern $canProtocolSource 'pau8CanData\[\(uint8_t\)\(u8ByteOffset\s*\+\s*1U\)\]\s*=\s*\(uint8_t\)\(\(u16Value\s*>>\s*8U\)\s*&\s*0xFFU\);' `
    'Signed CAN temperatures must retain little-endian serialization.'

$invalidTemperatureWireValue = [int16]::Parse('-400')
$invalidTemperatureWireWord = [uint16]($invalidTemperatureWireValue -band 0xFFFF)
if (($invalidTemperatureWireWord -band 0xFF) -ne 0x70 -or (($invalidTemperatureWireWord -shr 8) -band 0xFF) -ne 0xFE) {
    throw 'Invalid -40.0 C must serialize as little-endian bytes 70 FE.'
}

Write-Host 'PASS: temperature invalid sentinel contract'
