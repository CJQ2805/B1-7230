$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$addressHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.h')
$udsHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.h')
$udsPort = Get-Content -Raw -LiteralPath (Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.c')

if ($addressHeader -notmatch '(?s)#ifndef\s+DUCAR_CAN_BMS_A_ADDRESS\s+#define\s+DUCAR_CAN_BMS_A_ADDRESS\s+0x16U\s+#endif') {
    throw 'BMS source address must have a default of 0x16 and allow per-pack compile-time override.'
}
if ($udsHeader -notmatch 'UDS_CAN_REQUEST_ID_BASE\s+0x7E0U' -or
    $udsHeader -notmatch 'UDS_CAN_RESPONSE_ID_BASE\s+0x7E8U') {
    throw 'UDS physical request/response base IDs must be 0x7E0/0x7E8.'
}
if ($udsHeader -notmatch 'DUCAR_CAN_BMS_A_ADDRESS\s*-\s*0x16U' -or
    $udsHeader -notmatch 'UDS_CAN_REQUEST_ID_BASE\s*\+' -or
    $udsHeader -notmatch 'UDS_CAN_RESPONSE_ID_BASE\s*\+') {
    throw 'UDS physical IDs must be derived from the configured BMS address.'
}
if ($udsHeader -notmatch '(?s)#if\s*\(\s*DUCAR_CAN_BMS_A_ADDRESS\s*<\s*0x16U\s*\)\s*\|\|\s*\(\s*DUCAR_CAN_BMS_A_ADDRESS\s*>\s*0x1DU\s*\)\s*#error') {
    throw 'UDS mapping must reject BMS addresses outside 0x16..0x1D.'
}
if ($udsPort -match 'FUNCTION_ID|0x7DF') {
    throw 'UDS must not accept functional 0x7DF requests on the multi-PACK bus.'
}

$knownMappings = @(
    @{ Address = 0x16; Request = 0x7E0; Response = 0x7E8 },
    @{ Address = 0x17; Request = 0x7E1; Response = 0x7E9 },
    @{ Address = 0x1D; Request = 0x7E7; Response = 0x7EF }
)
foreach ($mapping in $knownMappings) {
    $offset = $mapping.Address - 0x16
    if ((0x7E0 + $offset) -ne $mapping.Request -or
        (0x7E8 + $offset) -ne $mapping.Response) {
        throw ('Incorrect expected UDS mapping for BMS address 0x{0:X2}.' -f $mapping.Address)
    }
}

if ($udsPort -notmatch 'if\s*\(\s*REQUEST_ID\s*==\s*id\s*\)' -or
    $udsPort -notmatch 'uds_tp_recv_frame\(\s*0\s*,') {
    throw 'Only the configured physical request ID should enter the UDS transport layer.'
}

Write-Output 'UDS CAN address contract: PASS'
