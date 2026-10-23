# N32G45x BOOT/APP 工程迁移 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在当前目录建立可独立打开、编译和生成 HEX 的 N32G45x BOOT 与 APP Keil MDK 工程。

**Architecture:** 以现有 N32WB4XX 工程为业务基线，复制 BOOT/APP 的业务和协议目录；每个工程内置 N32G45x CMSIS 与标准外设库，并在底层驱动和 Keil 工程文件中完成芯片替换。保持现有 512KB Flash IAP 分区，避免业务层和升级协议发生无关变化。

**Tech Stack:** Keil MDK/UV4、N32G45x Standard Peripheral Library 2.6.0、Cortex-M4 CMSIS、PowerShell 静态检查。

**Spec:** `docs/superpowers/specs/2026-08-21-n32g45x-boot-app-migration-design.md`

## Global Constraints

- 目标器件按 `N32G452REL7` 配置，编译宏为 `N32G45X, USE_STDPERIPH_DRIVER`。
- Flash 分区保持 BOOT `0x08000000`、APP1 `0x0800C000`、APP2 `0x08040000`、LOG `0x08070000`，总容量 512KB。
- Flash 页大小保持 2KB；BOOT、APP Flash map、IAP 和链接配置必须一致。
- `User/App`、协议和中间件不引入 N32G45x 厂家类型；芯片依赖集中在 Drivers、Board、Driver 和启动/中断层。
- 不复制 `.git`、Objects、Listings、JLink 用户配置和历史构建日志。

---

### Task 1: 建立独立 BOOT/APP 文件布局

**Files:**
- Create: `app/`、`boot/` 业务、Core、RTT、User 和工程目录
- Create: `docs/superpowers/specs/2026-08-21-n32g45x-boot-app-migration-design.md`
- Create: `docs/superpowers/plans/2026-08-21-n32g45x-boot-app-migration.md`

**Interfaces:**
- Consumes: `E:\DUCAR\new platform\DUCAR_N32WB4xx_Project\app` 和 `boot`
- Produces: 当前目录内不依赖源模板路径的 BOOT/APP 文件树

- [ ] **Step 1: 复制模板中不含生成物的目录**

复制 `Core`、`User`、`RTT` 和业务相关目录；排除 `.git`、`Objects`、`Listings`、`*.uvoptx`、`*.uvguix.*`、日志和 J-Link 用户配置。

- [ ] **Step 2: 检查复制后的源文件数量和路径**

运行：

```powershell
rg --files app boot -g '!**/Objects/**' -g '!**/Listings/**' | Measure-Object
```

预期：输出非零文件数量，且同时包含 `app/User`、`boot/User`、`app/Core`、`boot/Core`。

- [ ] **Step 3: 检查业务目录没有模板工程外部绝对路径**

运行：

```powershell
rg -n 'E:\\DUCAR|DUCAR_N32WB4xx_Project' app boot
```

预期：没有命中；工程文件中的路径在后续任务中改为当前目录相对路径。

### Task 2: 集成 N32G45x SDK 和底层适配

**Files:**
- Create: `app/Drivers/CMSIS/*`
- Create: `app/Drivers/N32G45X_HAL_Drivers/*`
- Create: `boot/Drivers/CMSIS/*`
- Create: `boot/Drivers/N32G45X_HAL_Drivers/*`
- Modify: `app/Core/inc/main.h`
- Modify: `app/Core/inc/n32g45x_it.h`
- Modify: `app/Core/src/n32g45x_it.c`
- Modify: `boot/Core/inc/main.h`
- Modify: `boot/Core/inc/n32g45x_it.h`
- Modify: `boot/Core/src/n32g45x_it.c`
- Modify: `app/User/Driver/*.c` and `boot/User/Hal_Driver/*.c`

**Interfaces:**
- Consumes: N32G45x Library 2.6.0 `firmware/CMSIS` and `firmware/n32g45x_std_periph_driver`
- Produces: `n32g45x.h`、`startup_n32g45x.s`、`system_n32g45x.c` 和 N32G45x 标准外设 API；上层驱动接口签名保持不变

- [ ] **Step 1: 复制 N32G45x CMSIS 和标准外设库**

从 `E:\DUCAR\new platform\国民\Nations.N32G45x_Library.2.6.0\Nations.N32G45x_Library.2.6.0` 复制 `firmware/CMSIS/core`、`firmware/CMSIS/device` 和 `firmware/n32g45x_std_periph_driver/inc/src` 到 APP、BOOT 两套 `Drivers` 目录。

- [ ] **Step 2: 迁移芯片绑定头文件和中断入口**

将 `n32wb452.h`、`n32wb452_it.h/.c` 的引用替换为 `n32g45x.h`、`n32g45x_it.h/.c`，并保留现有 SysTick、RTC 唤醒、AFE/串口 EXTI 分发逻辑。

- [ ] **Step 3: 适配 N32G45x 标准库差异**

以编译器错误为准只修改 `User/Driver`、BOOT `Hal_Driver`、`Core` 和 `Board`；不得把 N32G45x 类型扩散到 `User/App`。GPIO、USART、TIM、RTC、IWDG、FLASH、EXTI 和 NVIC 初始化保持现有上层行为。

- [ ] **Step 4: 检查芯片引用边界**

运行：

```powershell
rg -n -i 'n32wb452|N32WB4XX_HAL_Drivers' app boot
```

预期：只允许在迁移说明或历史文档中出现；可编译源文件和工程文件不得出现。

### Task 3: 生成 N32G45x BOOT/APP Keil 工程

**Files:**
- Create: `app/MDK-ARM/N32G45x.uvprojx`
- Create: `boot/Projects/MDK/boot.uvprojx`
- Create: `app/MDK-ARM/app.sct`
- Create: `boot/Projects/MDK/boot.sct`
- Modify: `boot/User/Boot/boot_config.h`
- Modify: `app/User/App/app_flash_map.h`
- Modify: `app/User/Middle/m_flash_wl.h`

**Interfaces:**
- Consumes: Task 1 的源文件和 Task 2 的 N32G45x SDK
- Produces: 可由 Keil 打开的独立工程，APP 链接到 `0x0800C000`，BOOT 链接到 `0x08000000`

- [ ] **Step 1: 从模板工程生成两份 XML**

复制模板的 `.uvprojx`，将 Device、PackID、Define、IncludePath、FilePath、启动文件、系统文件、目标名和输出名更新为 N32G45x 目录布局；APP 工程重命名为 `N32G45x.uvprojx`。

- [ ] **Step 2: 写入链接地址和输出配置**

APP Scatter 使用 `LR_IROM1 0x0800C000 0x00030000`，BOOT Scatter 使用 `LR_IROM1 0x08000000 0x0000B800`；RAM 按 N32G45x 512KB 目标的有效 SRAM 配置，并在工程 XML 中打开 HEX 输出。

- [ ] **Step 3: 对齐 Flash map**

确认 `boot_config.h`、`app_flash_map.h`、`m_flash_wl.h` 与 `0x08080000` 结束地址和 `0x800` 页大小一致，不能遗留 N32WB4XX 专用地址或页大小。

- [ ] **Step 4: XML 和源路径预检**

运行 PowerShell XML 检查，逐个验证 `<Device>`、`<Define>`、`<IncludePath>`、`<FilePath>` 和 `<ScatterFile>`；对每个 `<FilePath>` 解析相对于 `.uvprojx` 的绝对路径并确认存在。

### Task 4: 静态一致性验证

**Files:**
- Verify: `app/MDK-ARM/N32G45x.uvprojx`
- Verify: `boot/Projects/MDK/boot.uvprojx`
- Verify: `boot/User/Boot/boot_config.h`
- Verify: `app/User/App/app_flash_map.h`
- Verify: `app/User/Middle/m_flash_wl.h`

**Interfaces:**
- Consumes: Task 2 和 Task 3 的完整工程
- Produces: 无缺失源文件、无旧芯片工程引用、无分区边界矛盾

- [ ] **Step 1: 解析两个工程 XML**

使用 `[xml](Get-Content -Raw ...)` 读取两个 `.uvprojx`；解析失败即视为失败，不继续编译。

- [ ] **Step 2: 验证芯片和构建宏**

确认两个工程包含 `N32G452REL7`、`N32G45X` 和 `USE_STDPERIPH_DRIVER`，并引用 `n32g45x.h`、`startup_n32g45x.s`、`system_n32g45x.c`。

- [ ] **Step 3: 验证地址边界**

确认 BOOT、APP1、APP2、LOG 的起止地址与设计文档完全一致，并检查所有地址为 2KB 对齐。

### Task 5: 使用 Keil 重建并修复编译问题

**Files:**
- Modify: 仅限编译器报告的 `Core`、`User/Driver`、`User/Hal_Driver`、`Board` 和工程 XML
- Create: `app/MDK-ARM/rebuild.log`
- Create: `boot/Projects/MDK/rebuild.log`

**Interfaces:**
- Consumes: Task 4 通过的工程配置
- Produces: BOOT 和 APP 的成功构建结果及 HEX 文件

- [ ] **Step 1: 使用 Keil 重建 BOOT**

运行：

```powershell
& 'E:\Keil_v5\UV4\UV4.exe' -r 'C:\Users\chenc\OneDrive\文档\ChatGPT\DUCAR_N32G45x_Project\boot\Projects\MDK\boot.uvprojx' -t 'boot' -j0 -o 'C:\Users\chenc\OneDrive\文档\ChatGPT\DUCAR_N32G45x_Project\boot\Projects\MDK\rebuild.log'
```

预期：日志无 error，生成 BOOT ELF/HEX。

- [ ] **Step 2: 使用 Keil 重建 APP**

运行：

```powershell
& 'E:\Keil_v5\UV4\UV4.exe' -r 'C:\Users\chenc\OneDrive\文档\ChatGPT\DUCAR_N32G45x_Project\app\MDK-ARM\N32G45x.uvprojx' -t 'N32G45x' -j0 -o 'C:\Users\chenc\OneDrive\文档\ChatGPT\DUCAR_N32G45x_Project\app\MDK-ARM\rebuild.log'
```

预期：日志无 error，生成 APP ELF/HEX，map 中代码起始地址为 `0x0800C000`。

- [ ] **Step 3: 汇总并验证构建产物**

运行：

```powershell
Get-ChildItem app/MDK-ARM boot/Projects/MDK -Recurse -File | Where-Object { $_.Extension -in '.hex','.axf','.map' } | Select-Object FullName,Length
```

预期：BOOT、APP 各有非空 HEX 和 map/ELF 产物，且静态检查仍无旧芯片引用。
