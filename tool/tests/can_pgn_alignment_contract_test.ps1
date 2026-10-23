$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$HeaderPath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.h'
$SourcePath = Join-Path $ProjectRoot 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c'

$Header = Get-Content -Raw -LiteralPath $HeaderPath
$Source = Get-Content -Raw -LiteralPath $SourcePath

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

function Assert-NotMatch([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) {
        throw $Message
    }
}

# A0.2: F219 is the active charge-demand PGN; RTC is F229.
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F219\s+\(\(uint16_t\)0xF219U\)' 'F219 must remain the charge-demand PGN.'
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F229\s+\(\(uint16_t\)0xF229U\)' 'F229 must be defined for RTC.'

# A0.2 detailed active-frame table: F21A-C alarms and F21D-F protections.
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F21D\s+\(\(uint16_t\)0xF21DU\)' 'F21D protection PGN missing.'
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F21E\s+\(\(uint16_t\)0xF21EU\)' 'F21E protection PGN missing.'
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F21F\s+\(\(uint16_t\)0xF21FU\)' 'F21F protection PGN missing.'
# The on-demand F22E/F22F PGNs remain in the existing query table.
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F22E\s+\(\(uint16_t\)0xF22EU\)' 'F22E must be defined.'
Assert-Match $Header '#define\s+DUCAR_CAN_PGN_F22F\s+\(\(uint16_t\)0xF22FU\)' 'F22F must be defined.'
Assert-NotMatch $Header 'DUCAR_CAN_PGN_F22D' 'F22D is not an A0.2 active PGN.'

Assert-Match $Source 'ducar_can_send_f219' 'F219 charge-demand sender missing.'
Assert-Match $Source 'ducar_can_send_f21d' 'F21D protection sender missing.'
Assert-Match $Source 'ducar_can_send_f21e' 'F21E protection sender missing.'
Assert-Match $Source 'ducar_can_send_f21f' 'F21F protection sender missing.'

Assert-Match $Source 'case\s+DUCAR_CAN_PGN_F229\s*:' 'RTC query must use F229.'
Assert-Match $Source 'DUCAR_CAN_PGN_F229,\s*DUCAR_CAN_ACTION_NONE' 'RTC ACK must use F229.'
Assert-Match $Source 'DUCAR_CAN_PGN_F22F' 'Life-value query must use F22F.'
Assert-Match $Source 'DUCAR_CAN_PGN_F232' 'F232 log query missing.'

Write-Output 'CAN PGN alignment contract: PASS'
