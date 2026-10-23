param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$test = Join-Path $PSScriptRoot 'app_bms_cuv_temp_policy_test.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-cuv-temp-policy-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'app_bms_cuv_temp_policy_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$obj = Join-Path $build 'app_bms_cuv_temp_policy_test.obj'
$command = "cd /d `"$build`" && call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$sourceRoot`" /Fo`"$obj`" /Fe`"$exe`" `"$test`" && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Cell undervoltage temperature policy test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output 'PASS: cell undervoltage temperature bands, 1 C hysteresis, and invalid-temperature fallback.'
