param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\current_calib'
$test = Join-Path $PSScriptRoot 'current_calib_behavior_test.c'
$mathTest = Join-Path $PSScriptRoot 'current_calib_math_test.c'
$mathSupport = Join-Path $PSScriptRoot 'current_calib_math_test_support.c'
$implementation = Join-Path $sourceRoot 'App\app_current_calib.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-current-calib-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'current_calib_behavior_test.exe'
$mathExe = Join-Path $build 'current_calib_math_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "cd /d `"$build`" && call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /DAPP_CURRENT_CALIB_HOST_TEST /I`"$stubs`" /I`"$sourceRoot`" /Fe`"$exe`" `"$test`" `"$implementation`" && `"$exe`" && cl /nologo /std:c11 /utf-8 /W3 /DAPP_CURRENT_CALIB_HOST_TEST /I`"$stubs`" /I`"$sourceRoot`" /Fe`"$mathExe`" `"$mathTest`" `"$mathSupport`" `"$implementation`" && `"$mathExe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Current calibration behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output 'PASS: shared parameter-page calibration, two reserved pages, optional zero, shared K/B, and signed-point math.'
