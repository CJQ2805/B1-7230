# N32G45x FreeRTOS 系统层第 1 阶段验收记录

日期：2026-08-24

## 范围

本阶段只完成 FreeRTOS 系统层移植，不导入 BMS、AFE、Comm、DataHandle、SaveLog 五个业务任务。

- 芯片：`N32G452REL7`
- 内核：Cortex-M4F
- 工具链：ARMCLANG V6.16 / Keil MDK 5.36
- APP IROM：`0x0800C000`，大小 `0x30000`
- FreeRTOS 端口：`portable/GCC/ARM_CM4F`
- API 适配：CMSIS-RTOS v1
- 当前运行任务：`defaultTask` 加 FreeRTOS Idle task

## 验证命令和结果

### 1. 源码结构与阶段边界检查

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\app\MDK-ARM\freertos_system_source_check.ps1
```

结果：退出码 `0`。

检查内容包括：FreeRTOS 核心文件、CMSIS-RTOS 文件、Cortex-M4F 端口、Keil 工程文件路径、调度器启动、异常向量映射、原协作式业务调用仍在 `defaultTask` 中，以及五个新业务任务尚未导入。

### 2. Keil 全量重编译

```powershell
& "E:\Keil_v5\UV4\UV4.exe" -r ".\app\MDK-ARM\N32G45x.uvprojx" -t "N32G45x" -j0 -o ".\app\MDK-ARM\freertos_system_final_build.log"
```

结果：

```text
Program Size: Code=92812 RO-data=2692 RW-data=832 ZI-data=31240
".\Objects\N32G45x.axf" - 0 Error(s), 0 Warning(s).
```

输出文件：`app/MDK-ARM/Objects/N32G45x.axf`、`.bin`、`.hex`、`.htm` 均生成。

### 3. 调度器与异常向量核对

`app/MDK-ARM/Objects/N32G45x.htm` 中确认：

- `vTaskStartScheduler` 已链接；
- `SVC_Handler` 来自 `port.o`，并由启动文件引用；
- `PendSV_Handler` 来自 `port.o`，并由启动文件引用；
- `SysTick_Handler` 来自 `port.o`，并由启动文件引用；
- `app_main_legacy_task` 由 `defaultTask` 创建并运行。

FreeRTOS 配置中的映射为：

```c
#define vPortSVCHandler    SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler
```

依赖文件 `app/MDK-ARM/Objects/N32G45x_N32G45x.dep` 共 12 处引用本工程的 `..\Core\inc\FreeRTOSConfig.h`，未发现 `CF4872` 或 `N32WB4XX` 参考路径。

## 本阶段修改摘要

- 加入 FreeRTOS 核心、`heap_4`、CMSIS-RTOS v1 和 `GCC/ARM_CM4F` 端口；
- 新增 `FreeRTOSConfig.h`；
- 新增 `app_freertos.c/.h`，创建 `defaultTask` 并启动调度器；
- 移除应用中与 FreeRTOS 冲突的空 `SVC_Handler`、`SysTick_Handler`；
- 保留现有协作式业务循环，暂不做五任务拆分；
- 修正 CMSIS-RTOS 在 ARMCLANG 下的编译器头文件选择和新版任务通知 API 参数；
- 未修改 `boot` 工程源码、Flash map、业务协议、故障逻辑和低功耗策略。

说明：Keil 工程原有的 After Build 命令在本次编译后按既有配置刷新了 `tool/boot.hex` 和工厂 HEX 输出；这不是 BOOT 工程源码移植。

## 阶段结论

第 1 阶段 FreeRTOS 系统层移植验收通过。下一阶段才开始将现有循环按已确认的 5 个业务任务导入；在收到继续指令前不再修改业务任务代码。
