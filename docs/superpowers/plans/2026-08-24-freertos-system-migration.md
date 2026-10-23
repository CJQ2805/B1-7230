# N32G45x FreeRTOS 系统层移植 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 FreeRTOS 系统层接入 N32G452REL7 APP，并让现有协作式主循环在 `defaultTask` 中运行，为下一阶段导入 5 个业务任务建立可编译基础。

**Architecture:** 采用参考工程的 CMSIS-RTOS v1 API和文件组织，采用 N32G45 官方库中适配当前 ARMCLANG V6 工具链的 Cortex-M4F GCC 端口。第一阶段只启用一个 `defaultTask`，保留现有初始化和业务调度顺序；下一阶段再把该循环拆成 BMS、AFE、Comm、DataHandle、SaveLog 五个业务任务。

**Tech Stack:** Keil MDK/ARMCLANG V6.16, N32G452REL7, Cortex-M4F, FreeRTOS Kernel, CMSIS-RTOS v1, N32G45X standard peripheral library.

**Spec:** `docs/superpowers/specs/2026-08-24-freertos-system-migration-design.md`

## Global Constraints

- 目标芯片必须保持 `N32G452REL7`。
- APP 链接起始地址必须保持 `0x0800C000`。
- Cortex-M4F 必须使用 `GCC/ARM_CM4F` 端口，禁止使用参考工程的 `RVDS/ARM_CM0` 端口。
- 本阶段不导入 5 个业务任务，不修改业务协议、Flash map、故障逻辑和低功耗策略。
- Keil 编译结果必须为 0 Error(s), 0 Warning(s)。

---

### Task 1: 建立 FreeRTOS 依赖和配置基线

**Files:**
- Create: `app/Middlewares/Third_Party/FreeRTOS/Source/` 下的 FreeRTOS 核心、include、CMSIS_RTOS、portable 文件
- Create: `app/Core/inc/FreeRTOSConfig.h`
- Modify: `app/MDK-ARM/N32G45x.uvprojx`
- Test: `app/MDK-ARM/freertos_system_source_check.ps1`

**Interfaces:**
- Consumes: 参考工程的 CMSIS-RTOS v1 文件；N32G45 官方库 `GCC/ARM_CM4F` 端口。
- Produces: Keil 工程可找到 `FreeRTOS.h`、`cmsis_os.h`、`portmacro.h` 和全部核心实现。

- [ ] **Step 1: Write the failing source-structure test**

Create `app/MDK-ARM/freertos_system_source_check.ps1` that checks the required files and fails before the migration because the FreeRTOS source directory is absent:

```powershell
$projectRoot = Split-Path -Parent $PSScriptRoot
$required = @(
    "$projectRoot\Core\inc\FreeRTOSConfig.h",
    "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\tasks.c",
    "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\include\FreeRTOS.h",
    "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\CMSIS_RTOS\cmsis_os.c",
    "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\portable\GCC\ARM_CM4F\port.c",
    "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\portable\GCC\ARM_CM4F\portmacro.h"
)

$missing = $required | Where-Object { -not (Test-Path -LiteralPath $_) }
if ($missing.Count -ne 0) {
    $missing | ForEach-Object { Write-Host "MISSING: $_" }
    exit 1
}

if (Select-String -Path "$projectRoot\Middlewares\Third_Party\FreeRTOS\Source\portable\GCC\ARM_CM4F\port.c" -Pattern 'ARM_CM0' -Quiet) {
    Write-Host 'ERROR: CM0 port was selected for the Cortex-M4F target.'
    exit 1
}

exit 0
```

- [ ] **Step 2: Run test to verify it fails**

Run from the APP project root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\MDK-ARM\freertos_system_source_check.ps1
```

Expected: FAIL and report missing FreeRTOS files.

- [ ] **Step 3: Copy the minimal compatible source set**

Copy the kernel source files `croutine.c`, `event_groups.c`, `list.c`, `queue.c`, `stream_buffer.c`, `tasks.c`, `timers.c`, `portable/MemMang/heap_4.c`, the complete `Source/include` headers, and the reference `Source/CMSIS_RTOS/cmsis_os.c` plus `cmsis_os.h`. Copy only `port.c` and `portmacro.h` from the N32G45 official `GCC/ARM_CM4F` directory.

Create `FreeRTOSConfig.h` with the exact system values from the spec, including `configPRIO_BITS = __NVIC_PRIO_BITS`, `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`, `configUSE_PREEMPTION = 1`, `configTICK_RATE_HZ = 1000`, and the three exception handler mappings.

Modify the Keil project include path to add:

```text
..\Middlewares\Third_Party\FreeRTOS\Source\include
..\Middlewares\Third_Party\FreeRTOS\Source\CMSIS_RTOS
..\Middlewares\Third_Party\FreeRTOS\Source\portable\GCC\ARM_CM4F
```

Add the copied `.c` files to a `Middlewares/FreeRTOS` group in the Keil project.

- [ ] **Step 4: Run test to verify it passes**

Run the same PowerShell source-structure test and verify it exits with code 0, then parse the project XML to confirm all FreeRTOS source paths exist.

- [ ] **Step 5: Commit**

Do not commit in this workspace because the repository baseline is entirely untracked; leave the files in the shared workspace for user review.

### Task 2: Connect Cortex-M4F exception handlers and scheduler bootstrap

**Files:**
- Create: `app/User/App/app_freertos.c`
- Create: `app/User/App/app_freertos.h`
- Modify: `app/Core/src/main.c`
- Modify: `app/Core/src/n32g45x_it.c`
- Modify: `app/MDK-ARM/N32G45x.uvprojx`
- Test: `app/MDK-ARM/freertos_system_source_check.ps1`

**Interfaces:**
- Consumes: FreeRTOS kernel and CMSIS-RTOS v1 from Task 1.
- Produces: `app_freertos_init()` creates one `defaultTask`; `osKernelStart()` starts the scheduler; current main-loop behavior remains callable as `app_main_legacy_task()`.

- [ ] **Step 1: Write the failing integration assertions**

Extend `freertos_system_source_check.ps1` with these assertions:

```powershell
$main = Get-Content -Raw "$projectRoot\Core\src\main.c"
$interrupts = Get-Content -Raw "$projectRoot\Core\src\n32g45x_it.c"
$config = Get-Content -Raw "$projectRoot\Core\inc\FreeRTOSConfig.h"

if ($main -notmatch 'osKernelStart\s*\(\s*\)') { throw 'main.c does not start the scheduler' }
if ($main -notmatch 'app_freertos_init\s*\(\s*\)') { throw 'main.c does not initialize the FreeRTOS task layer' }
if ($config -notmatch 'xPortPendSVHandler\s+PendSV_Handler') { throw 'PendSV mapping is missing' }
if ($config -notmatch 'vPortSVCHandler\s+SVC_Handler') { throw 'SVC mapping is missing' }
if ($interrupts -match '(?m)^void\s+SVC_Handler\s*\(') { throw 'empty SVC_Handler conflicts with FreeRTOS port' }
if ($interrupts -match '(?m)^void\s+SysTick_Handler\s*\(') { throw 'empty SysTick_Handler conflicts with FreeRTOS port' }
```

- [ ] **Step 2: Run test to verify it fails**

Run the test before changing the application and verify it fails because `main.c` has no scheduler bootstrap and the interrupt file still owns empty system handlers.

- [ ] **Step 3: Implement the minimum scheduler bootstrap**

Create `app_freertos.h` with:

```c
#ifndef APP_FREERTOS_H
#define APP_FREERTOS_H

void app_freertos_init(void);

#endif
```

Create `app_freertos.c` with a CMSIS-RTOS v1 `defaultTask` definition that calls the externally defined `app_main_legacy_task()`:

```c
#include "cmsis_os.h"
#include "app_freertos.h"

extern void app_main_legacy_task(void const *argument);

osThreadId defaultTaskHandle;

void app_freertos_init(void)
{
    osThreadDef(defaultTask, app_main_legacy_task, osPriorityNormal, 0, 1024);
    defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);
}
```

In `main.c`, include `App/app_freertos.h`, replace the existing top-level `while (1)` with `app_freertos_init(); osKernelStart(); while (1) { }`, and move that loop body into `app_main_legacy_task(void const *argument)`. Add one `osDelay(1);` after `d_watchdog_feed()` so the bootstrap task yields.

In `n32g45x_it.c`, remove only the empty `SVC_Handler` and `SysTick_Handler`; leave HardFault and all peripheral handlers unchanged. Add `app_freertos.c` and `app_freertos.h` to the `User/App` Keil group.

- [ ] **Step 4: Run the structural test and Keil build**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\MDK-ARM\freertos_system_source_check.ps1
& "E:\Keil_v5\UV4\UV4.exe" -r ".\MDK-ARM\N32G45x.uvprojx" -t "N32G45x" -j0 -o ".\MDK-ARM\freertos_system_build.log"
```

Expected: structural test exits 0; Keil log ends with `0 Error(s), 0 Warning(s)`; map file contains the FreeRTOS scheduler and M4F port symbols.

- [ ] **Step 5: Inspect the generated map and dependency output**

Verify that the generated call-graph/map output contains `vTaskStartScheduler` and the mapped `SVC_Handler`, `PendSV_Handler`, and `SysTick_Handler` entries, and that the source dependency file includes `FreeRTOSConfig.h` from `app/Core/inc` rather than a reference-project path.

### Task 3: Phase-1 regression gate and handoff to business-task import

**Files:**
- Modify: `app/MDK-ARM/freertos_system_source_check.ps1`
- Create: `docs/superpowers/verification/2026-08-24-freertos-system-verification.md`

**Interfaces:**
- Consumes: the source structure, scheduler bootstrap, and build outputs from Tasks 1–2.
- Produces: a documented phase-1 verification record and a clean boundary for phase 2 business-task import.

- [ ] **Step 1: Add phase-boundary assertions**

Add checks that the current `main.c` still contains the existing calls `app_comm_task`, `app_bms_fault_task`, `app_h_power_service_task`, `hsi_afe_task`, `app_current_calib_task`, `app_soc_data_handle`, `app_mcu_ntc_task`, `app_balance_task`, `app_log_task`, and `app_low_power_task`, and that no `BMS_Task`, `AFE_Task`, `Comm_Task`, `DataHandle_Task`, or `SaveLog_Task` has been introduced yet.

- [ ] **Step 2: Run the complete phase-1 verification**

Run the structural test and a fresh Keil rebuild. Record the exact build result, program size, selected device, APP IROM range, FreeRTOS port path, and the fact that no BOOT files were modified.

- [ ] **Step 3: Write the verification handoff**

Create `docs/superpowers/verification/2026-08-24-freertos-system-verification.md` with the commands, exit codes, build log path, map-symbol checks, and a clear note that phase 2 has not yet imported the five business tasks.

- [ ] **Step 4: Stop at the phase boundary**

Do not modify business task files after the phase-1 verification. Report the exact files changed and wait for the user’s instruction before starting the 5-task import.
