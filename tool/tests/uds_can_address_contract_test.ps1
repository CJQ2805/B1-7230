$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ports = @(
    @{ Header = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.h';
       Source = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSBase\uds_port.c' },
    @{ Header = Join-Path $root 'boot\User\UDS\UDSBase\uds_port.h';
       Source = Join-Path $root 'boot\User\UDS\UDSBase\uds_port.c' }
)
foreach ($port in $ports) {
    $udsHeader = Get-Content -Raw -LiteralPath $port.Header
    $udsPort = Get-Content -Raw -LiteralPath $port.Source
    if ($udsHeader -notmatch 'REQUEST_ID\s+0x7A0U' -or
        $udsHeader -notmatch 'RESPONSE_ID\s+0x7A8U' -or
        $udsHeader -notmatch 'FUNCTIONAL_REQUEST_ID\s+0x7DFU') {
        throw "UDS CAN IDs must match the ZCANPRO configuration in $($port.Header)."
    }
    if ($udsPort -notmatch '(REQUEST_ID\s*==\s*id|id\s*==\s*REQUEST_ID)' -or
        $udsPort -notmatch '(FUNCTIONAL_REQUEST_ID\s*==\s*id|id\s*==\s*FUNCTIONAL_REQUEST_ID)' -or
        $udsPort -notmatch 'RESPONSE_ID') {
        throw "UDS transport must route 0x7A0 physical requests and 0x7DF functional requests in $($port.Source)."
    }
}

Write-Output 'UDS CAN address contract: PASS'
