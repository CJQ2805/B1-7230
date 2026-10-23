param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$test = Join-Path $PSScriptRoot 'app_h_power_detd_precharge_behavior_test.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-detd-precharge-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'app_h_power_detd_precharge_behavior_test.exe'
$obj = Join-Path $build 'app_h_power_detd_precharge_behavior_test.obj'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$sourceRoot`" /Fo`"$obj`" /Fe`"$exe`" `"$test`" && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "DETD/precharge behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "DETD/precharge behavior test artifacts: $build"
