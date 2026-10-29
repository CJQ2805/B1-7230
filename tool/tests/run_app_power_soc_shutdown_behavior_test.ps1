param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\power_shutdown'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-power-soc-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
foreach ($header in @('App/app_power_manage.h','FreeRTOS.h','task.h','App/app_bms_snapshot.h','App/app_h_power_manage.h','App/app_log_manage.h','App/app_low_power_manage.h','App/app_hw_apply.h','Driver/d_adc.h','Driver/d_system.h','Driver/d_timer.h','HSI_afe/hsi_afe.h','RTT/SEGGER_RTT.h','task/bms_task.h')) {
    $path = Join-Path $build $header
    $parent = Split-Path -Parent $path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Set-Content -LiteralPath $path -Value '#include "test_support.h"'
}
$exe = Join-Path $build 'test.exe'
$objDir = $build + '\\'
$test = Join-Path $PSScriptRoot 'app_power_soc_shutdown_behavior_test.c'
$module = Join-Path $sourceRoot 'App\app_power_manage.c'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$build`" /I`"$stubs`" /Fo`"$objDir`" /Fe`"$exe`" `"$test`" `"$module`" && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Shutdown SOC behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output 'Shutdown SOC behavior: PASS'
