# DUCAR CAN A0.3 Implementation Plan

> For agentic workers: use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 对齐已批准的 A0.3 报文字段、周期和 TP 传输，并在 param 标识协议版本。

**Architecture:** 保留现有普通 CAN 协议入口、业务参数和真实快照源。通信任务以 1ms 推进协议时基，主机心跳和 DETD 沿开启 5 秒周期报文窗口。

**Tech Stack:** C、PowerShell、MSVC 主机行为测试、Keil ArmClang 6.16。

**Spec:** `docs/superpowers/specs/2026-10-10-ducar-can-a03-alignment-design.md`

## Global Constraints

- 用户明确授权直接修改实际工程 main，保留现有未提交变更，不创建副本、不提交 Git。
- 保留 CHG-AFE-TOPCELL-MAP-20261010-01 的逻辑串数与末节映射。
- F200 T次版本按字段长度2和协议小端规则解释为U16。
- F218 BMR/ID 无已确认数据源：按未接入0报告，不再将负载检测/二级熔断检测冒充这两个信号。
- 无业务接口的查询/控制保留NAK；无统计源的协议量使用明确无效值。

## Review Focus

- T次版本超过255时不截断；极长版本数字饱和而不回绕。
- PC只发心跳且DETD不在位时能收到周期报文；窗口按5秒而非1秒计时。
- 全部主动报文的发送间隔与A0.3周期一致；静默与恢复不破坏计时。
- TP最后一帧DLC=8、有效字节后补FF；SN按RTS实际长度保存。
- F21D严重过压是布尔字段；充电过流保护输出真实最高等级。

## Task 1: 行为回归测试

Files: `tool/tests/can_a03_behavior_test.c`、`tool/tests/run_can_a03_behavior_test.ps1`、`tool/tests/host_stubs/cmsis_os.h`。

- [x] 用真实协议.c与真实业务头文件编译测试；仅替代硬件/业务外部接口。
- [x] 运行测试，确认F200、周期/心跳、TP尾帧及保护等级出现预期失败。

## Task 2: 参数、字段和时基

Files: `app_param.h`、`ducar_can_protocol.c`、`ducar_can_protocol.h`。

- [x] 新增协议版本参数0x03；F200按V/U16-T/HW/协议/保留输出。
- [x] 统一1ms时基，50/100/200/500/1000ms分组，F220=1000ms、F221=100ms。
- [x] F22F刷新5秒活跃窗口；ACK动作等待100ms、TP等待5秒。
- [x] 清除F218错误信号映射；F221无源报FFFF；纠正保护等级和过期注释。
- [x] TP尾帧固定DLC8并补FF。
- [x] 运行主机行为测试，预期所有测试通过。

## Task 3: 验证与交付

- [x] 运行已有CAN/拓扑/AFE相关测试，明确记录原有失败。
- [x] 实际Keil APP全量编译，读取新日志的错误、警告及链接结果。
- [x] 独立复核改动，记录无源/不支持项目与尚需实机联调的范围。
- [x] 更新本计划勾选状态和进度记录，交付真实修改文件及验证结果。
