param(
    [string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$app = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$udsBase = Join-Path $app 'Protocol\UDS\UDSBase'
$udsLogic = Join-Path $app 'Protocol\UDS\UDSLogic'
$appDir = Join-Path $app 'App'
$source = Join-Path $PSScriptRoot 'uds_version_security_behavior_test.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('uds-security-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'uds_version_security_behavior_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }

$sources = @(
    $source,
    (Join-Path $udsLogic 'SID22_ReadDataByIdentifier.c'),
    (Join-Path $udsLogic 'SID27_SecurityAccess.c'),
    (Join-Path $udsLogic 'uds_customer_profile.c')
)
$quotedSources = ($sources | ForEach-Object { '"{0}"' -f $_ }) -join ' '
$command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /I"{2}" /I"{3}" /I"{4}" /Fe"{5}" {6} /link /OPT:REF && "{5}"' -f `
    $VcVars, $app, $udsBase, $udsLogic, $appDir, $exe, $quotedSources
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) {
    throw "UDS version/security behavior test failed (exit $LASTEXITCODE); artifacts: $build"
}
Write-Output 'UDS version/security behavior test: PASS'
