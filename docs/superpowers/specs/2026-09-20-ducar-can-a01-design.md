# 已废弃：DUCAR CAN 通信协议 A0.1 通用设计

本文件不是 GD7627 同构移植方案，不能用于本次实施。请以 `2026-09-20-ducar-can-a01-gd7627-direct-port-design.md` 为唯一有效规格。

## 目标

在 N32G45x APP 工程中实现 `DUCAR_CAN通信协议A0.1.xlsx` 的 CAN 协议。结构和 7627-GD 工程的 `app_can.c`、`app_can_xingheng.c` 保持同类分层和 5ms 计数调度方式，同时保留 N32G45x 的 `d_can` 驱动与 `t_can_packet` 接口。

成功标准：

- CAN 接收入口先区分标准帧/扩展帧，再按协议类别分发；普通 DUCAR、CAN IAP、未来 UDS 的入口清晰可见。
- CAN 发送入口使用 GD7627 同类的 5ms 计数器、50/100/200/500/1000ms 分层任务和错相发送方式。
- A0.1 主动报文、按需报文、控制报文和 CAN IAP 控制边界明确，不在一个函数里混合处理。
- DETD 从未接入变为接入时，仅触发 5 秒主动发送窗口；上电和其他业务状态不会自行开启普通主动报文。
- 新增代码采用项目现有的匈牙利命名法，例如 `u8`、`u16`、`u32`、`pt`、`au8` 前缀；注释说明协议意义而非重复代码。

## 约束与来源

- 实施根目录固定为 `E:\DUCAR\DUCAR CODE\new platform\DUCAR_N32G45x_Project`。
- APP CAN 引脚保持当前硬件驱动的 PA11/CAN_RX、PA12/CAN_TX。
- 底层接收路径固定为 `d_can` 中断写入环形队列，再由 `d_can_read(t_can_packet *)` 取出。协议层不直接读 CAN FIFO。
- 当前 BMS 快照只支持 16 串，A0.1 定义 F20F–F214 共 24 串。第 1–16 串取快照值；第 17–24 串固定发送 `0xFFFF`，表示本硬件没有采样该串，不使用 `0x0000` 伪装成真实电压。
- CAN 2.0B 扩展帧为 DUCAR A0.1 正常协议；标准帧不被普通 DUCAR 协议误解析，留给后续 UDS 等协议入口。
- PC 源地址 `SA == 0x3A` 识别为 PC 报文来源。

## 模块边界

```text
CAN RX FIFO
  -> d_can ISR / RX ring (`t_can_packet`)
  -> app_can.c : 帧类型、协议类别判断和总调度
     -> ducar_can_protocol.c : 普通 DUCAR A0.1
     -> ducar_can_iap_protocol.c : APP 侧 CAN IAP 控制/转交
     -> User_Src/Protocol/UDS : 预留，当前不实现 UDS

BOOT CAN IAP 状态机继续位于 boot/User/Boot/boot_can_upgrade.c
```

### `User_Src/App/app_can.c/.h`

这是 APP CAN 的唯一总入口。

- `app_can_init()`：初始化 `d_can`，初始化普通协议与 CAN IAP 协议状态。
- `CAN_ReceiveTask()`：循环读取 `t_can_packet`，先按 `u32idType` 区分标准/扩展帧，再解码 29 位 ID。扩展帧通过 PF、目标地址、源地址分类后，明确调用普通协议、CAN IAP 或预留 UDS 分支。
- `CAN_TransmitTask()`：只编排两类协议的发送入口，首先调用 CAN IAP 发送服务，再调用普通 DUCAR 发送服务。不会保留 `CAN_TransmitNormalTask` 这类只转发一层的函数。

未来加入宗申或 UDS 时，只新增分类条件和各自协议文件；普通 DUCAR 解析函数不承载未知协议。

### `User_Src/Protocol/Comm/ducar_can_protocol.c/.h`

这里仅处理普通 DUCAR A0.1。

- 解码/编码 DUCAR 扩展 ID，按请求、控制、TP、主动报文 PGN 进行清晰的 `switch` 分发。
- 接收分支分别命名为按需报文、控制报文、TP 报文等处理函数。每个处理函数只处理一个协议类别。
- 发送分支把每个 A0.1 PGN 的组包放在独立函数，例如 `ducar_can_send_f20d()`；数据来自 `app_bms_snapshot`、故障、功率和数字输入快照，不跨层直接访问驱动私有变量。
- 不存在数据源的字段明确填 A0.1 规定的无效值或协议保留值，不自行推导业务值。F217 半充电次数的表格默认值为 `0x0000`，即使业务计数器尚不存在也发送 `0x0000`，不可套用通用 `0xFFFF` 无效值。

### `User_Src/Protocol/IAP/ducar_can_iap_protocol.c/.h`

这里只处理 APP 侧的 CAN IAP 协议识别、E1 控制命令响应以及进入 BOOT 的衔接。实际页接收、FLASH 擦写、CRC 校验和 APP2 搬运仍属于 BOOT 工程的 `boot_can_upgrade.c`，不复制到 APP 普通 CAN 协议中。

APP 需要复用现有 UART IAP 模块所拥有的升级状态页，而不是在 CAN 文件重复写 Flash。为此，UART IAP 模块会补一个窄接口：把升级标志写成 `0xBBBBBBBB`（等待 BOOT 收包），供 CAN IAP 的 E1/0x02 使用。

## CAN IAP 与 BOOT 的 A0.1 衔接

A0.1 中 E1/0x02 只有“开始 boot 升级”命令，字节 1–7 为保留字段；当前 BOOT 旧代码要求字节 1–4 带固件长度，两者不兼容。本次按 A0.1 统一，不保留该旧长度字段要求。

1. APP 收到来自 PC (`SA=0x3A`) 且目标为本机的 E1/0x01 时，回复 ACK，运行区标识为 `DUCAR_IAP_IN_APP`。
2. APP 收到 E1/0x02 时，写入 `0xBBBBBBBB` 等待包标志，发送 APP ACK，并在该 ACK 已提交 CAN 邮箱后的短延时复位。
3. BOOT 检测到 `0xBBBBBBBB` 后停留在 CAN 升级等待状态。PC 可通过 E1/0x01 查询，BOOT 回复 `DUCAR_IAP_IN_BOOT`。
4. BOOT 收到 A0.1 的 E1/0x02 后进入等待 TP RTS 状态；不读取 data[1..4] 作为固件长度。
5. TP CM/TP DP 首个完整 128 字节包到达后，BOOT 从包头解析固件长度、CRC 和品号，并将 TP 声明长度与解析结果交叉校验。校验通过后再持续接收 APP2 数据。
6. BOOT 收到完成命令并通过 CRC 校验后写入 `0xCCCCCCCC`，复位后执行 APP2 到 APP1 的搬运，最后回到 `0xAAAAAAAA` APP 有效状态。

因此，APP CAN IAP 只负责“切入 BOOT”；BOOT CAN IAP 才负责“接收升级包”。两者的 E1 ACK 字节格式保持 A0.1 示例：状态在 data[0]，运行区标识在 data[1]，被确认的 PF 在 data[6]。

## 接收流程

`CAN_ReceiveTask()` 的顺序固定如下：

1. 从 `d_can_read()` 取一帧完整 `t_can_packet`。
2. 判断标准帧或扩展帧。标准帧进入预留分支，当前不交给普通 DUCAR。
3. 对扩展帧解码 `priority / PF / PS / SA`，检查地址和 DLC。
4. `switch` 分发：CAN IAP、DUCAR 普通命令/请求/TP、未来 UDS、未知帧。
5. 未知帧和非法 DLC 只丢弃或计数，不影响后续队列报文。

这与 GD7627 的 `app_can_read()` 先识别报文类别、再交由对应模块的方式一致；差异仅在于 N32G45x 使用 `d_can` 环形队列而不是 `hsi_can` 缓冲区。

## 发送流程与周期

普通协议采用 GD7627 的 5ms 计数器架构。计数器滚动周期至少覆盖 1000ms，并在每种周期内使用错相槽位，每次服务最多发送一帧，避免同一时刻集中占用 CAN 邮箱。

| 周期 | A0.1 主动报文 |
| --- | --- |
| 50ms | F218 |
| 100ms | F20D、F20E、F21A、F21B、F21C、F21D、F221 |
| 200ms | F20F、F210、F211、F212、F213、F214 |
| 500ms | F215、F216、F217 |
| 1000ms | F220 |

发送调用关系固定为：

```text
Comm_Task (5ms)
  -> CAN_ReceiveTask()
  -> CAN_TransmitTask()
       -> ducar_can_iap_protocol_transmit_task()
       -> ducar_can_protocol_transmit_task()
            -> DETD 触发窗口维护
            -> 50ms / 100ms / 200ms / 500ms / 1000ms 子任务
```

`ducar_can_protocol_transmit_task()` 只在 DETD 接入沿触发的 5 秒窗口内主动报文。DETD 已持续在位、DETC 在位、MOS 状态、充放电状态、预放电状态、PC 状态都不作为新的主动发送启动条件。窗口结束后停止主动周期报文，直到下一次 DETD 重新接入。

## A0.1 数据映射原则

- F20D/F20E：电池包电压、电流、SOC、SOH、充电连接状态、允许电流和最大最小单体数据。
- F20F–F214：24 串单体电压，1–16 串来自快照，17–24 串为 `0xFFFF`。
- F215/F216/F217：温度、MOS 温度、循环次数、FCC、剩余容量等快照数据。
- F218：数字外部接入状态、MOS 状态和预留并联控制位。
- F21A/F21B/F21C/F21D/F220/F221：只能从已定义的限流、故障和统计数据源填充；没有现成数据源的字段按协议无效/保留值发送，不产生臆造的保护状态。
- F200/F201/F203/F205/F206/F208/F20B/F20C/F219/F21E/F21F：由请求/TP 分支按 A0.1 的地址、ACK 和传输控制格式回复。
- F800/F900/FA00/FB00/FC00/FE000：控制分支分别处理复位、电源、MOS、待机电流、调试和总线静默命令；E100 则按上面的 APP/BOOT 衔接进入 CAN IAP 模块。

## 错误处理

- 只接受格式正确、目标为本机或全局地址的协议帧。
- `d_can_send()` 返回失败时，本次发送不推进该报文槽位，下一轮重试；不使用阻塞等待。
- TP 收发的长度、包号、超时和 PGN 不匹配时中止当前会话并按协议回复失败状态。
- CAN IAP 和普通协议各自持有状态，不共享临时缓存，避免升级帧被普通 TP 误消费。

## 计划涉及的文件

- 新建 `app/N32G45x_KEIL/User_Src/App/app_can.c/.h`。
- 新建 `app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.c`，补全现有头文件接口。
- 新建 `app/N32G45x_KEIL/User_Src/Protocol/IAP/ducar_can_iap_protocol.c`，补全现有头文件接口。
- 更新 `Task/comm_task.c`、Keil `N32G45x.uvprojx` 的源文件组和包含关系。
- 新建 `User_Src/Protocol/UDS` 目录作为后续导入位置，不提供伪造的 UDS 实现。
- 仅在确认没有引用后移除遗留的 `app_can_xingheng.h`；不改动 `d_can` 的 FIFO/环形缓存职责。

## 验证

1. 先为 ID 编解码、帧分类、F20D/F20F/F218 组包、DETD 5 秒触发窗口编写可重复检查。
2. 用 Keil 对 APP 目标完整重新编译。
3. 用 CAN 工具验证主动报文在 DETD 接入后的 5 秒内按表中周期和错相发送，窗口结束后停止。
4. 验证 PC 地址 `0x3A` 请求、TP 长报文和 E1 CAN IAP 分类不会互相串帧。
