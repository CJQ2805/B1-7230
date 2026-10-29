param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-param-page-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$test = Join-Path $PSScriptRoot 'app_param_shared_page_behavior_test.c'
$module = Join-Path $src 'App\app_param.c'
$exe = Join-Path $build 'test.exe'
$obj = $build + '\\'
$command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /Gy /I"{1}" /Fo"{2}" /Fe"{3}" "{4}" "{5}" /link /OPT:REF && "{3}"' -f $VcVars,$src,$obj,$exe,$test,$module
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Shared parameter page behavior failed: $LASTEXITCODE" }
Write-Output 'Shared parameter page read-modify-write: PASS'
