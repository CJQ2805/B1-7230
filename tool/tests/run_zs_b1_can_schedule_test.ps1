param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$source = Join-Path $PSScriptRoot 'zs_b1_can_schedule_test.c'
$appCan = Join-Path $root 'app\N32G45x_KEIL\User_Src\App\app_can.c'
$includes = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$stubs = Join-Path $PSScriptRoot 'host_stubs'
$build = Join-Path ([IO.Path]::GetTempPath()) ('zs-b1-can-schedule-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'zs_b1_can_schedule_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
Push-Location $build
try {
    $command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /I"{2}" /Fe"{3}" "{4}" "{5}" && "{3}"' -f $VcVars, $stubs, $includes, $exe, $source, $appCan
    & $env:ComSpec /d /c $command
}
finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) { throw "宗申协议调度测试失败 (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "宗申协议调度测试产物: $build"
