param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$includes = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$source = Join-Path $PSScriptRoot 'zs_b1_current_threshold_contract_test.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('zs-b1-current-threshold-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$exe = Join-Path $build 'zs_b1_current_threshold_contract_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
Push-Location $build
try {
    $command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /Fe"{2}" "{3}" && "{2}"' -f $VcVars, $includes, $exe, $source
    & $env:ComSpec /d /c $command
}
finally {
    Pop-Location
}
if ($LASTEXITCODE -ne 0) { throw "宗申/低功耗电流门限联动测试失败 (exit $LASTEXITCODE); artifacts: $build" }
Write-Output 'Zongshen/low-power current threshold contract: PASS'
