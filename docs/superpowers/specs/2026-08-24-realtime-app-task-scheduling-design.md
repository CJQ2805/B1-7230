# APP 实时任务调度设计

## 目标

重新设计 APP 的 FreeRTOS 任务节拍，不继承原工程的 500ms/1000ms 慢周期；将采样、快照、保护和业务处理改为确定性实时调度，同时保持低功耗和日志功能语义正确。

## 设计

- `AFE_Task`：10ms 固定周期，执行一步 AFE 服务、当前校准、均衡和低功耗状态机。
- `DataHandle_Task`：10ms 固定周期，执行 MCU NTC 采样和 `app_bms_snapshot_update()`；LED 使用内部计数器控制可见翻转频率。
- `BMS_Task`：10ms 固定周期，只消费快照，执行故障、MOS 和 SOC；不再直接刷新快照。
- `Comm_Task`：5ms 固定周期，及时排空 UART 接收缓存。
- `SaveLog_Task`：10ms 固定周期，快速响应日志队列和外部状态变化；Flash 每次最多写一条记录。
- `Default_Task`：10ms 固定周期，保持后台任务统一实时节拍。
- 所有周期任务使用 `osDelayUntil()`，避免任务执行时间造成节拍漂移。
- 低功耗入口改为接收毫秒增量，内部以毫秒累计 30s/1s 判定，不再假设“一次调用等于一秒”。
- 快照改为 `app_bms_snapshot_read()` 复制读取，使用短临界区保证 DataHandle 写入与其他任务读取的一致性，不恢复 FreeRTOS Mutex；临界区只覆盖结构体复制。
- 均衡判断每 10ms 执行，但只有请求变化时才向 AFE 写 IIC，避免高频重复写入。

## 任务优先级

`AFE_Task=osPriorityRealtime`，`DataHandle_Task=osPriorityHigh`，`Comm_Task=osPriorityAboveNormal`，`BMS_Task=osPriorityNormal`，`SaveLog_Task=osPriorityBelowNormal`，`Default_Task=osPriorityLow`。

## 范围约束

- 只修改 `app\N32G45x_KEIL` APP 源码、APP Keil 工程和 APP 检查脚本。
- 不修改 `boot` 目录及 BOOT 工程。
- 不重新引入资源 Mutex/lock/unlock。
