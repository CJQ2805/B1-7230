$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$logic = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic'
$readDid = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID22_ReadDataByIdentifier.c')
$security = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID27_SecurityAccess.c')
$profileHeader = Get-Content -Raw -LiteralPath (Join-Path $logic 'uds_customer_profile.h')
$profileSource = Get-Content -Raw -LiteralPath (Join-Path $logic 'uds_customer_profile.c')
$serviceConfig = Get-Content -Raw -LiteralPath (Join-Path $logic 'service_cfg.c')

function Assert-Contains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
}

# F180/F193 use the customer's six-byte version record, with fixed 00.01 values.
Assert-Contains $readDid "(?s)rsp_buf\[3\]\s*=\s*'B'.*?rsp_buf\[4\]\s*=\s*'V'.*?rsp_buf\[5\]\s*=\s*':'.*?rsp_buf\[6\]\s*=\s*0U.*?rsp_buf\[7\]\s*=\s*'\.'.*?rsp_buf\[8\]\s*=\s*1U.*?uds_positive_rsp\(rsp_buf,\s*9U\)" 'F180 must respond as BV:00.01.'
Assert-Contains $readDid "(?s)rsp_buf\[3\]\s*=\s*'H'.*?rsp_buf\[4\]\s*=\s*'V'.*?rsp_buf\[5\]\s*=\s*':'.*?rsp_buf\[6\]\s*=\s*0U.*?rsp_buf\[7\]\s*=\s*'\.'.*?rsp_buf\[8\]\s*=\s*1U.*?uds_positive_rsp\(rsp_buf,\s*9U\)" 'F193 must respond as HV:00.01.'

# F189 returns the customer's six-byte DataRecord: ASCII "SV:", numeric V, '.', numeric T.
Assert-Contains $readDid "(?s)case 0xF189U:.*?app_param_get_package_v_version\(\).*?app_param_get_package_t_version\(\).*?rsp_buf\[3\]\s*=\s*'S'.*?rsp_buf\[4\]\s*=\s*'V'.*?rsp_buf\[5\]\s*=\s*':'.*?rsp_buf\[6\]\s*=\s*major.*?rsp_buf\[7\]\s*=\s*'\.'.*?rsp_buf\[8\]\s*=\s*minor.*?uds_positive_rsp\(rsp_buf,\s*9U\)" 'F189 must return the six-byte customer SV version record.'

# Customer SecurityAccess has an Extended-session LV1 pair and a Programming-session LV2 pair.
foreach ($subfunction in '0x01', '0x02', '0x09', '0x0A') {
    Assert-Contains $security ([regex]::Escape($subfunction)) "SecurityAccess subfunction $subfunction is missing."
}
Assert-Contains $security 'get_current_session\(\)' 'SecurityAccess must validate the active diagnostic session.'
Assert-Contains $security 'UDS_SESSION_EXT' 'SecurityAccess must bind level 1 to Extended Session.'
Assert-Contains $security 'UDS_SESSION_PROG' 'SecurityAccess must bind level 2 to Programming Session.'
Assert-Contains $security 'pending.*seed|seed.*pending' 'SecurityAccess must bind a submitted key to its preceding seed request.'
Assert-Contains $security 'UDS_SA_LV2' 'A successful 27 09/0A exchange must unlock level 2.'
Assert-Contains $security '(?s)uds_positive_rsp\(rsp_buf,\s*2U\)\s*==\s*0\).*?set_current_sa_lv\(requested_level\)' 'Security level must not unlock unless the positive response is queued.'
Assert-Contains $security '(?s)session == UDS_SESSION_EXT.*?UDS_LV1_REQUEST_SEED.*?UDS_LV1_SEND_KEY.*?\*level_out = UDS_SA_LV1' 'Extended Session must map only 27 01/02 to level 1.'
Assert-Contains $security '(?s)session == UDS_SESSION_PROG.*?UDS_LV2_REQUEST_SEED.*?UDS_LV2_SEND_KEY.*?\*level_out = UDS_SA_LV2' 'Programming Session must map only 27 09/0A to level 2.'
Assert-Contains $security 'subfunction != expected_key_subfunction' 'A key must match the pending seed subfunction pair.'
Assert-Contains $profileHeader '(?m)^#define\s+UDS_CUSTOMER_KEY_CONST_VALID\s+1U' 'Security access must be enabled with verified customer DLL constants.'
Assert-Contains $profileSource '(?s)0xACU,\s*0x89U,\s*0x18U,\s*0xF2U.*?0xE3U,\s*0xBBU,\s*0x25U,\s*0x40U' 'DLL KeyConst values for SecurityAccess levels 1 and 9 must both be present.'
Assert-Contains $security 'pending_seed_subfunction\)\s*==\s*0' 'SecurityAccess must select the key constant using the seed subfunction.'

$sessionControl = Get-Content -Raw -LiteralPath (Join-Path $logic 'SID10_SessionControl.c')
Assert-Contains $sessionControl 'uds_security_reset\(\)' 'Changing diagnostic sessions must clear the security level and pending seed.'

$rows = $serviceConfig -split '\r?\n'
$sid27 = $rows | Where-Object { $_ -match '\{SID_27\s*,' } | Select-Object -First 1
if ($sid27 -notmatch 'FALSE\s*,\s*TRUE\s*,\s*TRUE\s*,.*UDS_SA_NON') {
    throw 'SID 27 must be available in both Programming and Extended sessions.'
}
foreach ($sid in 'SID_2E', 'SID_31', 'SID_34', 'SID_36', 'SID_37') {
    $row = $rows | Where-Object { $_ -match ('\{' + $sid + '\s*,') } | Select-Object -First 1
    if ($row -notmatch 'UDS_SA_LV2') { throw "$sid must require customer SecurityAccess level 2." }
}

Write-Output 'Customer version and SecurityAccess contract: PASS'
