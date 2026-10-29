param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs\led'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-led-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$test = Join-Path $PSScriptRoot 'app_led_charge_behavior_test.c'
$module = Join-Path $src 'App\app_led.c'
$exe = Join-Path $build 'test.exe'
$obj = $build + '\\'
$command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /I"{2}" /Fo"{3}" /Fe"{4}" "{5}" "{6}" && "{4}"' -f $VcVars,$stubs,$src,$obj,$exe,$test,$module
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "LED charge behavior failed: $LASTEXITCODE" }
Write-Output 'LED charge behavior: PASS'
