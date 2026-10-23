param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$bootUser = Join-Path $root 'boot\User'
$bootBase = Join-Path $bootUser 'UDS\UDSBase'
$bootLogic = Join-Path $bootUser 'UDS\UDSLogic'
$appLogic = Join-Path $root 'app\N32G45x_KEIL\User_Src\Protocol\UDS\UDSLogic'
$source = Join-Path $PSScriptRoot 'boot_uds_upgrade_driver_behavior_test.c'
$module = Join-Path $bootLogic 'uds_upgrade.c'
$build = Join-Path ([IO.Path]::GetTempPath()) ('boot-uds-driver-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
$objDir = Join-Path $build 'obj'
New-Item -ItemType Directory -Path $objDir | Out-Null
$objDirWithSlash = $objDir + '\\'
$exe = Join-Path $build 'boot_uds_upgrade_driver_behavior_test.exe'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = 'call "{0}" >nul && cl /nologo /std:c11 /utf-8 /W3 /I"{1}" /I"{2}" /I"{3}" /I"{4}" /Fo"{5}" /Fe"{6}" "{7}" "{8}" /link /OPT:REF && "{6}"' -f `
    $VcVars, $bootUser, $bootBase, $bootLogic, $appLogic, $objDirWithSlash, $exe, $source, $module
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "BOOT UDS FlashDriver behavior test failed (exit $LASTEXITCODE); artifacts: $build" }
Write-Output "BOOT UDS FlashDriver behavior test artifacts: $build"
