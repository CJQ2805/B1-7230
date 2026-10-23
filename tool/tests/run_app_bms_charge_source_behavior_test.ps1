param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\charge_fault'
$test = Join-Path $PSScriptRoot 'app_bms_charge_source_behavior_test.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-charge-source-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'app_bms_charge_source_behavior_test.exe'
$obj = Join-Path $build 'app_bms_charge_source_behavior_test.obj'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /Gy /I`"$stubs`" /I`"$sourceRoot`" /Fo`"$obj`" /Fe`"$exe`" `"$test`" /link /OPT:REF && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Charge-source behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "Charge-source behavior test artifacts: $build"
