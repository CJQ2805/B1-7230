# BMS 功能配置统一设计

## 目标

为 N32G452REL7 APP 工程建立统一的 BMS 保护配置入口 `app_bms_config.h`，集中管理保护类开关；均衡继续使用现有 `app_balance_manage.h` 配置，保证关闭功能时对应的任务和硬件动作真正停止或进入安全状态。

本次设计保持当前工程的有效默认值不变，不重新解释或调整现有保护阈值。现有配置文件中发现的“注释值与宏值不一致”问题另行审核，不能在本次迁移中静默修改。

## 现状与边界

当前配置分散在以下位置：

- `app_bms_fault_config.h`：保护总开关、保护子开关、阈值和延时。
- `app_balance_manage.h`：均衡开关和均衡阈值。
- `app_param.h`：软件版本、产品型号、硬件版本、电流零点死区等整机参数。
- `FreeRTOSConfig.h`：RTOS 内核配置。

本次只统一 BMS 业务配置，不把 RTOS 内核参数、Flash 分区、产品身份参数或底层驱动常量搬入 BMS 配置文件。

## 方案

### 1. 唯一配置入口

新增：

`app/User/App/app_bms_config.h`

该文件按以下顺序组织：

1. 保护总开关与保护子开关；
2. 每个开关的风险和默认值说明；
3. 0/1 配置值的编译期校验。

当前默认值保持如下：

| 配置项 | 默认值 | 作用 |
|---|---:|---|
| `BMS_FEATURE_FAULT_PROTECT_ENABLE` | `1U` | 软件保护总功能 |
以下功能不再提供顶层开关，直接作为 APP 固定功能启用：

- MOS 高压服务；
- 低功耗管理；
- 普通 BMS 通信；
- IAP 通信；
- 电流校准；
- 故障/运行日志；
- MCU NTC 采样；
- LED 状态指示；
- SOC 计算。

均衡不属于 `app_bms_config.h`，继续保留在已有的 `app_balance_manage.h`：

- `APP_BALANCE_ENABLE`：`1U`；
- `APP_BALANCE_START_DIFF_MV`：`30U`；
- `APP_BALANCE_STOP_DIFF_MV`：`10U`；
- `APP_BALANCE_MIN_CELL_MV`：`3200U`。

保护类开关从 `app_bms_fault_config.h` 移入该文件，名称和值保持原样，包括 AFE 故障、NTC、单体/总压、充放电温度、充放电过流、短路、负载检测和单体压差保护开关。旧名称 `BMS_SW_PROTECT_ENABLE` 通过兼容映射保留，实际值由 `BMS_FEATURE_FAULT_PROTECT_ENABLE` 提供。

### 2. 配置文件职责

`app_bms_config.h`：保护总开关和保护子开关。

`app_bms_fault_config.h`：保护阈值、恢复阈值、动作延时；引用 `app_bms_config.h`，不再重复定义保护开关。

`app_balance_manage.h`：均衡开关、均衡阈值和均衡模块接口；沿用现有文件，不新增均衡配置文件。

`app_param.h`：产品身份参数、电流零点死区等整机参数，保持独立。

`FreeRTOSConfig.h`：内核时基、堆、任务调度等 RTOS 参数，保持独立。

### 3. 功能接入点

统一配置必须改变实际执行路径，而不能只新增宏：

- `main.c`：MOS 高压服务、MCU NTC、日志、通信和电流校准始终初始化；不为这些固定功能添加配置守卫。
- `app_business_tasks.c`：MOS 服务、低功耗、普通通信、IAP、日志、电流校准、MCU NTC、LED 和 SOC 始终执行；任务保持周期运行并喂狗。
- `app_comm_manage.c`：普通 BMS 协议和 IAP 始终启用，不再由顶层功能宏分流关闭。
- `app_log_manage.c`：日志始终初始化并允许故障/运行快照写入 Flash。
- `app_mcu_ntc.c`/故障统计：MCU NTC 始终采样，现有 NTC 保护配置继续决定是否参与保护。
- `app_balance_manage.c`：均衡开关关闭时主动清除 AFE 均衡位图并报告非活动状态；启用时使用 `app_balance_manage.h` 中的三个均衡阈值。
- `app_low_power_manage.c`：低功耗始终允许按现有策略进入 STOP。
- MOS 服务：始终运行现有 MOS 控制服务，启动时仍保持既有安全初始化状态。

### 4. 安全约束

- 本次迁移默认不改变任何现有开关值和阈值。
- 所有开关只允许 `0U` 或 `1U`，使用编译期检查阻止非法值。
- AFE 采样和 BMS 核心任务不提供普通可关闭开关；它们是保护链基础设施。
- 关闭均衡必须清除硬件均衡输出。
- MOS 服务、低功耗、通信、IAP、电流校准、日志、MCU NTC、LED 和 SOC 不提供关闭开关，避免出现功能链路被部分关闭的状态。

## 验证方案

1. PowerShell 静态检查：确认所有配置项只在统一入口定义，旧配置文件不再重复定义，并确认每个顶层开关在实际调用点存在守卫。
2. 默认配置 Keil 编译：目标为 `N32G452REL7`，APP 工程零错误、零警告。
3. 代码审查：核对保护默认宏值与迁移前值逐项一致；核对 `app_balance_manage.h` 中的均衡阈值为 30mV/10mV/3200mV。
4. Map 检查：确认任务入口、调度器和关键业务符号仍被链接。
5. 输出配置清单：记录所有开关当前值、所在模块、关闭后的安全行为和未处理的注释/宏值矛盾。

## 验收标准

- 工程存在 `app_bms_config.h`，保护开关可在该文件修改；均衡开关和均衡阈值仍在已有 `app_balance_manage.h` 修改。
- `app_bms_fault_config.h` 不再重复定义保护开关。
- 不新增均衡配置文件，`app_balance_manage.h` 继续承担均衡配置和接口职责。
- 可配置功能关闭后，实际初始化或任务行为符合安全约束；固定功能始终存在于初始化和任务路径。
- 当前默认配置与迁移前的有效行为保持一致。
- Keil APP 工程编译通过，静态检查和 Map 检查通过。
