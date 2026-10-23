# N32G45x FreeRTOS 业务任务第 2 阶段验收记录

日期：2026-08-24

## 任务划分

| 任务 | 周期 | 当前接管内容 | 优先级 |
| --- | ---: | --- | --- |
| `BMS_Task` | 10ms | 快照、故障判断、MOS 服务；每 25 次执行一次 SOC | Normal |
| `AFE_Task` | 100ms | AFE 分步采样、电流校准；每 1s 执行均衡和低功耗 | AboveNormal |
| `Comm_Task` | 5ms | BMS 协议、IAP 协议和 LOG 导出 | High |
| `DataHandle_Task` | 500ms | 运行 LED；每 1s 执行 MCU NTC 采样 | BelowNormal |
| `SaveLog_Task` | 1000ms | LOG 状态检查和 Flash 落盘 | BelowNormal |
| `defaultTask` | 100ms | 系统空闲占位，不承载业务 | Low |

`AFE_Task` 独占 AFE 任务链路，保证采样、均衡和低功耗操作在同一任务上下文中串行执行。

## 共享资源保护

业务任务新增三类 CMSIS-RTOS mutex：

- AFE mutex：保护 BMS、通信和 AFE 任务对软件 I²C/AFE 控制的访问；
- LOG mutex：保护故障快照入队、LOG 查询和 LOG 消费；
- Flash mutex：保护电流校准保存、IAP Flash 操作和 LOG 落盘。

锁顺序统一为 `AFE -> LOG -> Flash`；`SaveLog_Task` 使用 `LOG -> Flash`，未发现反向获取路径。

## 验证命令和结果

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\app\MDK-ARM\freertos_system_source_check.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\app\MDK-ARM\freertos_business_tasks_source_check.ps1
```

结果：两个检查均退出码 `0`。

```powershell
& "E:\Keil_v5\UV4\UV4.exe" -r ".\app\MDK-ARM\N32G45x.uvprojx" -t "N32G45x" -j0 -o ".\app\MDK-ARM\freertos_business_final_build.log"
```

结果：

```text
Program Size: Code=98316 RO-data=2828 RW-data=832 ZI-data=31280
".\Objects\N32G45x.axf" - 0 Error(s), 0 Warning(s).
```

最终组合验收确认：

- 芯片仍为 `N32G452REL7`；
- APP IROM 仍为 `0x0800C000`，大小 `0x30000`；
- Map 中存在 `BMS_Task`、`AFE_Task`、`Comm_Task`、`DataHandle_Task`、`SaveLog_Task`、`app_default_task`；
- Map 中存在 `vTaskStartScheduler` 以及 `SVC_Handler`、`PendSV_Handler`、`SysTick_Handler`；
- Keil 工程 XML 中所有源文件路径均存在；
- 没有修改 BOOT 工程源码、业务协议、Flash map、故障阈值和低功耗策略。

构建输出：`app/MDK-ARM/Objects/N32G45x.axf`、`.bin`、`.hex`、`.htm`。

## 代码变更

- 新增 `app_business_tasks.c/.h`；
- 新增 `app_freertos_resources.c/.h`；
- `app_freertos.c` 从单一 `defaultTask` 改为创建五个业务任务和一个默认任务；
- `main.c` 只保留系统初始化和调度器启动，不再直接执行业务循环；
- Keil `User/App` 组加入新增任务和资源锁文件；
- 新增业务任务结构检查脚本。

本阶段只完成源码编译、链接、Map 和静态边界验收，尚未连接实物板进行任务运行、看门狗、低功耗唤醒和 I²C 时序测试。
