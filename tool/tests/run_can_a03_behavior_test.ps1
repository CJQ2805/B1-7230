param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Join-Path $PSScriptRoot 'can_a03_behavior_test.c'
$includes = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-can-a03-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'can_a03_behavior_test.exe'
$obj = Join-Path $build 'can_a03_behavior_test.obj'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$stubs`" /I`"$includes`" /Fo`"$obj`" /Fe`"$exe`" `"$source`" && `"$exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "CAN behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "CAN behavior test artifacts: $build"
