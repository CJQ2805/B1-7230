param(
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$generator = Join-Path (Split-Path -Parent $PSScriptRoot) 'generate_bms_ducar_flash_driver.py'
$artifact = Join-Path (Split-Path -Parent $PSScriptRoot) 'BMS-DUCAR_FlashDriver.bin'

& $Python $generator
if ($LASTEXITCODE -ne 0) {
    throw "FlashDriver generator failed with exit code $LASTEXITCODE"
}
if (!(Test-Path -LiteralPath $artifact)) {
    throw "Expected generated FlashDriver file is missing: $artifact"
}

[byte[]]$actual = [System.IO.File]::ReadAllBytes($artifact)
if (($actual.Length -ne 9) -or
    ([System.BitConverter]::ToString($actual) -cne '42-4D-53-2D-44-55-43-41-52')) {
    throw "FlashDriver must be exactly nine bytes: BMS-DUCAR; actual=$($actual.Length) bytes"
}

Write-Output 'UDS FlashDriver artifact contract: PASS'
