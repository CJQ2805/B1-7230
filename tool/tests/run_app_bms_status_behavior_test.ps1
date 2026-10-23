param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\bms_status'
$test = Join-Path $PSScriptRoot 'app_bms_status_behavior_test.c'
$module = Join-Path $sourceRoot 'App\app_bms_status.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-bms-status-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$objDir = Join-Path $build 'obj'
New-Item -ItemType Directory -Path $objDir | Out-Null
$objDirWithSlash = $objDir + '\\'
$exe = Join-Path $build 'app_bms_status_behavior_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$stubs`" /I`"$sourceRoot`" /Fo`"$objDirWithSlash`" /Fe`"$exe`" `"$test`" `"$module`" /link /OPT:REF && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "BMS status behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "BMS status behavior test artifacts: $build"
