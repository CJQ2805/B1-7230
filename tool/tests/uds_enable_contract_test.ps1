$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$param = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_param.h')
$paramValidation = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_param_validate.h')
$can = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_can.c')
$comm = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\task\comm_task.c')
$port = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.c')
$serviceCfg = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic\service_cfg.c')
$serviceCfgHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic\service_cfg.h')
$session = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic\SID10_SessionControl.c')
$readDid = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic\SID22_ReadDataByIdentifier.c')
$security = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic\SID27_SecurityAccess.c')
$udsPortHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.h')
$project = [xml](Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx'))

if ($param -notmatch '(?m)^#define\s+APP_PARAM_UDS_ENABLE\s+1U\s*$') {
    throw 'APP_PARAM_UDS_ENABLE must be enabled in app_param.h for this product configuration'
}
if ($paramValidation -notmatch '(?s)#if\s*\(\s*APP_PARAM_UDS_ENABLE\s*!=\s*0U\s*\)\s*&&\s*\(\s*APP_PARAM_UDS_ENABLE\s*!=\s*1U\s*\).*?#error') {
    throw 'APP_PARAM_UDS_ENABLE must reject values other than 0U and 1U'
}
foreach ($token in 'uds_init\(\)', 'uds_recv_frame\(', 'APP_PARAM_UDS_ENABLE') {
    if ($can -notmatch $token) { throw "APP CAN lifecycle gate is missing: $token" }
}
if ($comm -notmatch '(?s)#if\s*\(\s*APP_PARAM_UDS_ENABLE\s*==\s*1U\s*\).*?uds_1ms_task\(\).*?#endif') {
    throw 'UDS 1 ms task must run only while UDS is enabled'
}
if ($port -match 'FlexCAN\.h|FLEXCAN0_transmit_msg') {
    throw 'UDS still uses the legacy FlexCAN transmit adapter'
}
if ($port -notmatch '(?s)d_can_send\(\s*&can0\s*,\s*response_id\s*,\s*D_CAN_ID_STANDARD') {
    throw 'UDS responses must be sent as standard frames through d_can'
}
if ($serviceCfgHeader -notmatch '(?m)^#define\s+SID_NUM\s+5\b') {
    throw 'APP UDS service table must contain only the handoff and pre-handoff services'
}
foreach ($sid in 'SID_10', 'SID_22', 'SID_27', 'SID_28', 'SID_85') {
    if ($serviceCfg -notmatch "\{\s*$sid\s*,") { throw "Approved UDS service is not registered: $sid" }
}
foreach ($sid in 'SID_11', 'SID_2E', 'SID_31', 'SID_34', 'SID_36', 'SID_37') {
    if ($serviceCfg -match "\{\s*$sid\s*,") { throw "APP must not expose BOOT-owned upgrade service: $sid" }
}
foreach ($sid in 'SID_14', 'SID_18', 'SID_19', 'SID_2F') {
    if ($serviceCfg -match "\{\s*$sid\s*,") { throw "Unrequested UDS service remains registered: $sid" }
}
if ($session -match 'EraseFlash|WriteFlash|SystemSoftwareReset|system_S32K144|Flash\.h') {
    throw 'UDS session control still contains the legacy platform Flash/reset path'
}
if ($session -notmatch '(?s)#define\s+P2_SERVER\s+100\b' -or
    $session -notmatch '(?s)#define\s+P2X_SERVER\s+400\b' -or
    $session -notmatch '(?s)case\s+UDS_SESSION_PROG\s*:.*?set_current_session\s*\(\s*\(uds_session_t\)subfunction\s*\).*?uds_positive_rsp\s*\(\s*rsp_buf\s*,\s*6\s*\)') {
    throw 'Programming session must be enabled with P2=100 ms and P2*=4000 ms'
}
if ($udsPortHeader -notmatch '(?m)^#define\s+PADDING_VAL\s+0xFF\b') {
    throw 'UDS ISO-TP padding must use 0xFF'
}
if ($readDid -notmatch "(?s)case\s+0xF189U:.*?app_param_get_package_v_version\(\).*?app_param_get_package_t_version\(\).*?rsp_buf\[3\]\s*=\s*'S'.*?rsp_buf\[4\]\s*=\s*'V'.*?rsp_buf\[5\]\s*=\s*':'.*?rsp_buf\[6\]\s*=\s*major.*?rsp_buf\[7\]\s*=\s*'\.'.*?rsp_buf\[8\]\s*=\s*minor.*?uds_positive_rsp\(rsp_buf,\s*9U\)") {
    throw 'F189 must return the configured customer SV DataRecord with numeric version components'
}
if ($security -match 'static\s+uint8_t\s+rand_u8\s*\(\s*void\s*\)\s*\{\s*//') {
    throw 'Optional SecurityAccess seed generator must return a defined seed byte'
}

$requiredSources = @(
    'uds_port.c', 'uds_service.c', 'uds_tp.c', 'service_cfg.c',
    'SID10_SessionControl.c', 'SID22_ReadDataByIdentifier.c',
    'SID27_SecurityAccess.c', 'SID28_CommunicationControl.c',
    'SID85_ControlDTCSetting.c',
    'uds_customer_profile.c'
)
$files = $project.SelectNodes('//File')
foreach ($name in $requiredSources) {
    $entry = $files | Where-Object { $_.FileName -eq $name } | Select-Object -First 1
    if ($null -eq $entry) { throw "Required UDS source is missing from Keil project: $name" }
    if ($entry.FileOption.CommonProperty.IncludeInBuild -ne '1') {
        throw "Required UDS source is excluded from Keil build: $name"
    }
}
$excludedSources = @(
    'SID11_EcuReset.c', 'SID2E_WriteDataByIdentifier.c',
    'SID31_RoutineControl.c', 'SID34_RequestDownload.c',
    'SID36_TransferData.c', 'SID37_RequestTransferExit.c',
    'uds_upgrade.c', 'SID3E_TesterPresent.c',
    'SID14_ClearDiagnosticInformation.c',
    'SID19_ReadDTCInformation.c'
)
foreach ($name in $excludedSources) {
    $entry = $files | Where-Object { $_.FileName -eq $name } | Select-Object -First 1
    if ($null -eq $entry) { throw "UDS project entry is missing: $name" }
    if ($entry.FileOption.CommonProperty.IncludeInBuild -ne '0') {
        throw "Unrequested UDS source must stay excluded: $name"
    }
}
$includePath = [string]$project.Project.Targets.Target.TargetOption.TargetArmAds.Cads.VariousControls.IncludePath
foreach ($directory in '..\User_Src\Protocol\UDS\UDSBase', '..\User_Src\Protocol\UDS\UDSLogic') {
    if ($includePath -notmatch [regex]::Escape($directory)) {
        throw "Keil include path is missing UDS headers directory: $directory"
    }
}
Write-Output 'UDS upgrade enable contract: PASS'
