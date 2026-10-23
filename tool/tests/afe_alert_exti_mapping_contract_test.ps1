$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$AppRoot = Join-Path $ProjectRoot 'app/N32G45x_KEIL'
$BoardPath = Join-Path $AppRoot 'User_Src/Board/board_io.h'
$InterruptPath = Join-Path $AppRoot 'Core/src/n32g45x_it.c'
$BoardText = Get-Content -Raw -LiteralPath $BoardPath
$InterruptText = Get-Content -Raw -LiteralPath $InterruptPath

$AlertPinMatch = [regex]::Match($BoardText, '(?m)^\s*#define\s+BOARD_IO_AFE_ALERT_PIN\s+D_GPIO_PIN\((\d+)U\)')
$IrqHandlerMatch = [regex]::Match(
    $InterruptText,
    '(?s)void\s+EXTI15_10_IRQHandler\s*\(\s*void\s*\)\s*\{(?<body>.*?)\}')
$DispatchMatch = [regex]::Match(
    $IrqHandlerMatch.Groups['body'].Value,
    'd_exti_irq_handler\s*\(\s*D_GPIO_PIN\((\d+)U\)\s*\)')

$AlertPinMatchesDispatcher = $AlertPinMatch.Success -and
    $IrqHandlerMatch.Success -and
    $DispatchMatch.Success -and
    ($AlertPinMatch.Groups[1].Value -eq $DispatchMatch.Groups[1].Value)

if (-not $AlertPinMatchesDispatcher)
{
    throw 'AFE ALERT GPIO pin and EXTI15_10 IRQ dispatcher pin do not match; the actual EXTI pending bit may remain uncleared.'
}

Write-Output 'PASS: AFE ALERT pin matches the EXTI15_10 IRQ dispatcher pin'
