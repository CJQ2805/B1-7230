# BMS LOG 代码同步说明（供其他项目 Codex 使用）

**整理日期：2026-10-27**
**用途：**把本工程已经完成的 LOG 行为修改迁移到其他采用相同 BMS / FreeRTOS / Flash wear-leveling 架构的 APP 工程。本文是同步规格，不要求照抄本工程的芯片参数、Flash 地址或完整源文件。

## 给接手任务的 Codex

先阅读目标项目的 `AGENTS.md`、LOG 结构定义、Flash 存储接口、BMS 故障触发点、启动顺序和 RTT 配置。确认目标项目对应模块后，按本文“应同步的行为”逐项修改；不要整文件覆盖，也不要覆盖用户已有改动。完成后列出实际改动文件和验证结果。

本说明只包含本工程已经落地的修改。SoftwareWarning 的预留位扩展已落地，按下文位序同步；不要再把这些有效位清零。

## 已同步的 LOG 行为

### 触发条件

实时 LOG 继续采用“事件产生快照、后台任务写 Flash”的结构。实际触发来源为：

| 原因值 | 来源 | 行为 |
|---:|---|---|
| 1 | BMS 故障位出现新置位 | 入队一条快照 |
| 2 | BMS 故障位清除 | 入队一条快照 |
| 3 | 同一轮既有置位又有清除 | 入队一条快照 |
| 5 | 发现非纯上电复位来源 | 入队一条独立复位记录 |
| 6 | 软关机流程 | 入队关机快照 |

不再因 MOS 或外部状态变化保存 LOG，也不生成固定周期快照。正常上电过程中的充电、预放电、放电 MOS 切换不应新增记录。MOS 状态仍保存在其他事件产生的快照内。

`LOG_TRIGGER_SIGNAL_CHANGE` 的数值 4 保留在枚举中，以便解释旧记录；新的 `app_log_request_snapshot(LOG_TRIGGER_SIGNAL_CHANGE)` 请求被拒绝。不可重排或复用已有原因编号。

快照先入 RAM 环形队列，由 10 ms LOG 任务每轮最多提交一条至 Flash。沿用目标项目的队列锁、Flash wear-leveling 和失败处理方式。

### 复位来源只记录一次

在启动早期、芯片/系统驱动清除 reset flags 之前读取复位来源，并通过 RTT 输出原始寄存器值及各来源解码。当前工程在 `main()` 的 `SEGGER_RTT_Init()` 之后、外设初始化之前调用 `app_log_capture_reset_flags()`。

BOR、软件复位、IWDG、WWDG、低功耗复位，以及不伴随 POR 的 NRST 复位，会自动生成一条独立的 `LOG_TRIGGER_RESET` 记录。纯上电 POR（含仅伴随 PINRST 的 POR）不自动存 LOG，避免正常上电写入。复位记录只带复位来源，不合并硬件故障、软件告警或软件故障位。

复位来源 pending 只填入 `LOG_TRIGGER_RESET` 记录，并且只有该记录 Flash append 成功后才清除；任何后续告警/故障日志的复位位保持0。若多个 RCC 复位标志在启动捕获时同时置位，独立复位记录按原始标志如实显示多个来源。

### RTT 输出

保留启动早期 `RESET` 输出。`app_log_init()` 扫描 LOG Flash 区后输出 `[start,end)`、容量、页数和初始化状态；这只是诊断输出，不得格式化或擦除历史日志。

Flash append 成功后才输出该记录的详细内容，包括：

- `LOG_SAVED`：原因名称/编号、事件时间，以及实际提交时的 tick；RTC 不可用时使用 uptime。
- `LOG_DATA`：SOC、电芯电压摘要、总压、电流和温度。
- `LOG_FLAGS`：硬件故障、软件告警/故障、信号、复位、SOC、时钟、队列溢出和 checksum。
- `LOG_RAW`：按偏移分行打印实际 `sizeof(log_rtd_t)` 个原始字节；每行最多 16 字节。

LOG 外部信号位 `ex_sig.bit.detd_stu` / `detc_stu`（Bit0 / Bit1）必须从统一快照的 `signal.input.mcu_detd` / `mcu_detc` 读取。它们是板级输入经过消抖后的状态，1 表示在位；不可硬编码为0，也不可用 MOS 状态代替。DETD/DETC 仅作为其他事件日志的快照内容，不触发日志。RTT 的 `LOG_FLAGS` 除输出 `signals` 位图外，也单独显示 `detd`、`detc`。修改固件只影响之后新保存的记录，不会回填历史记录。

Flash append 失败时输出 `LOG_WRITE_FAILED`、触发原因及底层状态码；不得打印 `LOG_SAVED`。输出完整记录的时机必须保持在 append 成功之后。

当前 RTT 上行缓冲从 1024 调整为 4096 字节，channel 0 仍为 `SEGGER_RTT_MODE_NO_BLOCK_SKIP`。迁移时根据目标 RTT 实现和其他启动输出量确认缓冲容量；不得为避免丢 RTT 而无评估地改成阻塞输出。

### NTC 诊断（不改变告警/保护策略）

LOG 任务首次取得统一 BMS snapshot 时建立诊断基准并输出一次 `NTC_SAMPLE`；之后仅在 NTC `fault_mask` 或异常通道数量变化时再次输出。每次输出包含异常数量、fault/valid/short/open 掩码及 MCU 侧 NTC ADC raw 值。当前掩码约定是 AFE=bit0、CELL1=bit1、CELL2=bit2、MOS=bit3、PTC=bit4；目标工程应以自己的公共类型定义为准。

DVC1124 的 AFE NTC 采样状态变化时，驱动输出 `NTC_AFE`：tick、状态码、寄存器读取是否成功、V1P8/GP3 原始值、trim 和温度。IIC 失败时 `reads_ok=0`，寄存器镜像可能过期，不能把旧值当成本次电气故障证据。若目标工程不是 DVC1124 或寄存器/AFE NTC 通道不同，只迁移诊断意图，按目标驱动接口重写字段。

若快照捕获时 NTC 异常数量非零，`LOG_NTC_CAPTURE` 输出这条待保存快照的触发原因、异常数量和通道诊断值。

这些输出只用于定位异常来源。本次修改没有改 NTC 阈值、保护延时、有效性判定或告警策略，也没有加入三次采样去抖。不要顺手把诊断输出变成告警屏蔽逻辑。

### 故障位快照的一致性

生成一条日志时只读取一次 `app_bms_snapshot_t`，并将同一份 snapshot 传给软件告警和软件故障采集函数。迁移时避免这两个 collector 各自重读快照，导致同一条日志的温度/NTC 状态来自不同采样周期。

### SoftwareWarning 协议位扩展

协议 Bit0~7 顺序保持不变：放电过流、普通充电过流、放电高温、充电高温、放电低温、充电低温、MOS过温、NTC告警。Bit8~13 使用原预留位，按以下顺序分别表达此前被合并的来源：

| 位 | 告警 |
|---:|---|
| 8 | 单体欠压 |
| 9 | 总压欠压 |
| 10 | 单体过压 |
| 11 | 总压过压 |
| 12 | 单体压差 |
| 13 | 回馈过流 |

Bit14~31 仍为预留并发送0。日志内部 `sw_warn` 保持32位，复用原未使用位保存总压欠/过压和回馈过流；`reserved_store[0]` 写入映射版本1。协议导出根据该标记区分新旧Flash记录：旧记录继续按原有合并位序输出，不将历史汇总值误解为单体或总压独立告警。迁移时若已有Flash日志，也应保留该兼容分支。

### 删除未实现的 `b1pack_war_roc`

`app_bms_fault.h` 中原 `b1pack_war_roc` 没有置位实现，不是真实可触发的独立告警。当前移除了该名称，并将原位改为 `b1reserved_roc_war`，保留原 bit 位，避免后续位域偏移；日志充电电流告警汇总也不再引用它。

实际存在的回馈过流告警是 `b1pack_war_orcc`，不要将它与未实现的 `b1pack_war_roc` 混为一项。目标项目若没有同样的死字段/同样的证据，不要盲目删自己的字段。

`b1pack_pro_roc` 同样没有 APP 置位来源；当前实际回馈过流保护为 `b1pack_pro_orcc`。本工程将 `b1pack_pro_roc` 改为 `b1reserved_roc_pro`，保持原位号，并从 LOG 故障汇总和 ZS B1 CAN 保护判断中移除该无效条件。迁移前应在目标工程搜索确认没有独立置位来源。

## 按目标项目迁移时要保留的约束

1. 保持目标项目现有 `log_rtd_t` 持久化布局、原因编号、checksum 算法和底层槽位格式。参考工程为 70 字节 payload、92 字节 store slot；这两个数不能直接覆盖目标项目定义。
2. 不复制本工程 Flash 起始地址、页数、芯片时钟、AFE 地址、NTC 通道、阈值和延时。通过目标项目已有配置/API 获取。
3. 不格式化 LOG 区，不删除或改写已有 Flash 记录。
4. 不因 MOS 快照字段仍存在，就恢复 MOS 状态变化触发日志。
5. 不把本工程 `app_bms_snapshot.c` 中与 LOG 无关的业务参数修改一并迁移。
6. 保持 RTT diagnostics 与持久化记录分层：Flash 初始化信息可以启动时打印，记录内容只能在 append 成功后打印。

## 参考工程文件

在目标项目内先找相同职责文件，不要求路径完全相同。

| 参考路径 | 同步职责 |
|---|---|
| `app/N32G45x_KEIL/User_Src/App/app_log_manage.c` | 触发过滤、reset pending、NTC 诊断、append 后 RTT 完整输出 |
| `app/N32G45x_KEIL/User_Src/App/app_log_manage.h` | 原因编号兼容、记录结构大小、告警位及映射版本 |
| `app/N32G45x_KEIL/User_Src/App/app_bms_fault.c` | 仅故障图置位/清除边沿请求日志；软关机调用保留 |
| `app/N32G45x_KEIL/User_Src/App/app_bms_fault.h` | 未实现 `b1pack_war_roc` 删除并以保留位维持布局 |
| `app/N32G45x_KEIL/Core/src/main.c` | RTT 初始化后、其他初始化前捕获 reset flags |
| `app/N32G45x_KEIL/User_Src/task/save_log_task.c` | 10 ms 后台任务调用 `app_log_task()` |
| `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe_dvc1124.c` | 同款 DVC1124 AFE NTC 状态及原始寄存器变化诊断 |
| `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_uart_protocol.c` | 0x49软件告警位映射及旧Flash记录兼容 |
| `app/N32G45x_KEIL/User_Src/RTT/SEGGER_RTT_Conf.h` | RTT 上行缓冲容量 |
| `tool/tests/app_log_behavior_test.c`、`tool/tests/run_app_log_behavior_test.ps1` | 参考行为测试：MOS 时序不存日志、纯POR不存日志、复位独立且只存一次、队列溢出、append 失败和软关机 |

## 同步完成标准

- 纯 MOS 状态切换不增加 Flash LOG 条数；故障置位/清除、非纯上电复位独立记录和软关机记录仍按原因触发。
- 每次启动的 reset source 最多出现在一条成功 append 的记录中；append 失败不会提前消费该来源。
- RTT 只在 append 成功后输出 `LOG_SAVED` 和完整 payload；失败只输出失败诊断。
- NTC/AFE 原始诊断能区分通道和采样/IIC 状态，且不改变告警与保护结果。
- 持久化结构、Flash 分区及目标项目已有历史记录保持兼容。
- 按目标项目工程约定运行相关行为测试和完整编译，并检查产物及构建后脚本造成的工作区变化。
