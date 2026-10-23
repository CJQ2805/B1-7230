# N32 软关机流程设计

日期：2026-10-13  
工程：`DUCAR_N32G45x_Project`

## 目标

CAN 和 UART 软关机命令统一进入完整关机流程：记录带关机标记的实时 LOG，禁止休眠；等待 15 秒；强制关闭充电、放电和预放 MOS；确认三路状态均关闭后保存关机时快照；让 DVC1124 AFE 进入 SHUTDOWN；关闭 MCU ADC 与板级 12V 使能。拉低 12V 使能后若 MCU 仍运行 3 秒，则调用工程的 `d_system_reset()`。

## 当前实现依据

- CAN `PF=E2` 与 UART `0x66` 都在 ACK 完成后执行关机动作，目前动作只是拉低 `DCDC_EN`。
- BMS 任务周期为 10 ms，适合作为非阻塞状态机的驱动位置；通信任务、AFE 任务和日志任务在等待期间继续运行。
- 低功耗调用当前在 BMS 任务中注释掉，但关机期间仍要显式禁止休眠，避免以后重新启用低功耗时影响流程。
- AFE 状态寄存器提供 CMOS/DMOS 反馈；预放 MOS 没有独立反馈输入，工程已有状态只能反映板级输出命令。
- MCU ADC 抽象层目前只有 `d_adc_init()` / `d_adc_read()`，需增加关闭 ADC 的驱动接口；由于读接口会自动初始化 ADC，关机后还需锁住 ADC，直到 MCU 复位。
- `AFE_Task` 在 SHUTDOWN 命令之后仍会执行采样、电流校准和均衡，因此必须在发出 SHUTDOWN 后禁止这三类 AFE 访问。
- `board_io.h` 将 PC4 定义为 `DCDC_EN`，用户已确认它就是流程图中的 12V 使能。
- `app_log_manage.h` 已定义 `log_sw_err_t.bit.power_off_f`，但当前 LOG 采集函数没有给该位赋值。字段已在实时 LOG 结构中，无需扩展记录长度。

## 流程与状态机

1. CAN/UART 在 ACK 发送完成后调用统一的软关机请求接口；接口幂等，避免重复命令重新开始计时。
2. 请求被接收时，关机管理器禁止低功耗，并请求一条软关机实时 LOG。该记录的 `sw_err.bit.power_off_f` 置 1，触发类型使用专门的关机枚举值。此位只写入 LOG，不改写 BMS 实际故障状态。
3. 关机管理器用毫秒时基非阻塞等待 15 秒。期间正常任务继续运行。
4. 15 秒到期后，以高于校准、远程控制等来源的优先级强制请求三路 MOS 关闭，防止普通控制逻辑重新打开 MOS。
5. 继续轮询状态：AFE `status_valid` 有效且 AFE 反馈的充电、放电 MOS 均关闭；预放 MOS 的业务输出镜像也为关闭。三项满足后才进入下一步。反馈无效或任一 MOS 仍打开时保持在此状态，不提前关闭 AFE、ADC 或 12V。
6. 请求一条最终实时快照，同样置 `sw_err.bit.power_off_f`，等待日志任务完成写入；日志写入失败或超时采用有界等待，随后继续关机，避免日志故障让电池长期保持运行。
7. 调用 `hsi_afe_shutdown()` 获取 AFE 资源锁、发送 DVC1124 SHUTDOWN 命令并锁存 AFE 软件关机状态。`AFE_Task` 看到该状态后跳过采样、电流校准和均衡；随后调用新增的 `d_adc_deinit()` 关闭 ADC 外设和 ADC 时钟，并锁住 ADC 自动重初始化，最后将 PC4 `DCDC_EN` 拉低。
8. 拉低 `DCDC_EN` 后等待 3 秒；如果 MCU 仍在执行，则调用 `d_system_reset()` 作为掉电失败后的兜底复位。

## 模块边界

- 新增 App 软关机管理器，负责状态、定时、睡眠抑制、MOS 关断请求和末段动作顺序。
- CAN/UART 只负责协议校验、ACK 完成与提交请求，不承担延时，也不直接切电。
- 高压管理器通过 `MF_Poweroff` 强制三路 MOS 关闭；该来源必须具有最高优先级。
- LOG 管理器识别关机触发，构造 `power_off_f` 标记并提供 pending 写入状态，以便最终快照有界等待。
- ADC 关闭封装留在 `Driver/d_adc.c`；电源脚控制仍通过 `app_hw_apply_dcdc_enable()` 和 Board 适配层完成。

## 兼容与失败处理

- `log_rtd_t` 的 70 字节业务结构和 Flash 槽长度不变；`power_off_f` 使用现有软件故障位字段。
- 关机命令 ACK 行为维持现状，ACK 发送完成后才启动 15 秒计时。
- 三路 MOS 未确认关闭时不执行 AFE SHUTDOWN 或切断 ADC/12V，以保留反馈和控制能力。
- AFE SHUTDOWN 和 ADC 关闭后不再执行依赖这些外设的业务步骤。
- 12V 拉低后的 3 秒兜底复位使用项目封装 `d_system_reset()`。

## 实现文件范围

- `app/N32G45x_KEIL/User_Src/App/app_log_manage.c/.h`
- `app/N32G45x_KEIL/User_Src/App/app_h_power_manage.c/.h`
- `app/N32G45x_KEIL/User_Src/App/app_low_power_manage.c/.h`
- 新增 `app/N32G45x_KEIL/User_Src/App/app_power_manage.c/.h`
- `app/N32G45x_KEIL/User_Src/Driver/d_adc.c/.h`
- `app/N32G45x_KEIL/User_Src/HSI_afe/hsi_afe.c/.h`
- `app/N32G45x_KEIL/User_Src/task/afe_task.c`
- `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c`
- `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_uart_protocol.c`
- `app/N32G45x_KEIL/User_Src/task/bms_task.c`
- Keil 工程文件中登记新增 App 源文件。

## 设计审查清单

- [x] 15 秒等待不阻塞协议、AFE、日志或 BMS 调度。
- [x] MOS 关断优先级高于其他强制控制来源。
- [x] 充电/放电状态来自有效 AFE 反馈；预放按用户确认的软件输出镜像判断。
- [x] 软关机 LOG 置位既有 `log_sw_err_t.bit.power_off_f`，不改变故障状态和存储布局。
- [x] 12V 使能按用户确认映射到 PC4 `DCDC_EN`。
- [x] AFE SHUTDOWN 后停止 AFE 采样、电流校准和均衡；ADC 关闭后不会被读接口自动重初始化。
- [x] 最终快照写入采用有界等待，日志异常不会永久阻塞掉电。
