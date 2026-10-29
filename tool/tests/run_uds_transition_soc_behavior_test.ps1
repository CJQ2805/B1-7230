param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat', [switch]$Baseline)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\uds_transition'
$build = Join-Path ([IO.Path]::GetTempPath()) ('uds-soc-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
foreach ($header in @('uds_port.h','App/app_can.h','Driver/d_can.h','Driver/d_system.h','Driver/d_timer.h','uds_service.h','uds_tp.h','Task/bms_task.h')) {
    $path = Join-Path $build $header
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
    Set-Content -LiteralPath $path -Value '#include "test_support.h"'
}
$module = Join-Path $build 'uds_port.c'
Copy-Item -LiteralPath (Join-Path $src 'Protocol\UDS\UDSBase\uds_port.c') -Destination $module
if ($Baseline) {
    $module = Join-Path $build 'baseline_uds_port.c'
    & git -C $root show 'HEAD:app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSBase/uds_port.c' | Set-Content -LiteralPath $module -Encoding UTF8
}
$exe = Join-Path $build 'test.exe'
$obj = $build + '\\'
$test = Join-Path $PSScriptRoot 'uds_transition_soc_behavior_test.c'
$command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /I"{2}" /Fo"{3}" /Fe"{4}" "{5}" "{6}" && "{4}"' -f $VcVars,$build,$stubs,$obj,$exe,$test,$module
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "UDS SOC transition behavior failed: $LASTEXITCODE" }
Write-Output 'UDS SOC transition behavior: PASS'
