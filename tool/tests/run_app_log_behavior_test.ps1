param([string]$VcVars = 'E:\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$sourceRoot = Join-Path $root 'app\N32G45x_KEIL\User_Src'
$build = Join-Path ([IO.Path]::GetTempPath()) ('ducar-log-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $build | Out-Null
@'
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
'@ | Set-Content -LiteralPath (Join-Path $build 'FreeRTOS.h')
@'
#pragma once
#include "FreeRTOS.h"
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, TickType_t t) { (void)m; (void)t; return 1; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m) { (void)m; return 1; }
'@ | Set-Content -LiteralPath (Join-Path $build 'semphr.h')
@'
#pragma once
#define taskSCHEDULER_NOT_STARTED 0
static inline int xTaskGetSchedulerState(void) { return 1; }
'@ | Set-Content -LiteralPath (Join-Path $build 'task.h')
$test = Join-Path $PSScriptRoot 'app_log_behavior_test.c'
$topology = Join-Path $PSScriptRoot 'app_cell_topology_test.c'
if (!(Test-Path -LiteralPath $VcVars)) { throw "MSVC environment not found: $VcVars" }
$command = "call `"$VcVars`" >nul && cl /nologo /std:c11 /utf-8 /W3 /I`"$build`" /I`"$sourceRoot`" /Fo`"$build\log.obj`" /Fe`"$build\log.exe`" `"$test`" && `"$build\log.exe`" && cl /nologo /std:c11 /utf-8 /W3 /I`"$sourceRoot`" /Fo`"$build\topology.obj`" /Fe`"$build\topology.exe`" `"$topology`" && `"$build\topology.exe`""
& $env:ComSpec /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Log/topology behavior tests failed; artifacts: $build" }
Write-Output "PASS: log and cell topology behavior tests; artifacts: $build"
