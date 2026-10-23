# DUCAR CAN A0.1 GD7627 同构移植实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 N32G45x APP 与 BOOT 中实现 `DUCAR_CAN通信协议A0.1`，以 GD7627 的 `app_can.c` / `app_can_xingheng.c` 为直接母版，保留其收发入口、5ms 计数器、模运算与错相 `if / else if` 槽位结构；同时使用 G45x 的 `d_can` 原始帧环形缓存和 N32 硬件适配。

**Architecture:** `d_can` 只负责 PA11/PA12 CAN 硬件、ISR 和原始 `t_can_packet` 环形缓存；`app_can` 只负责一次取帧后的帧类别分流与 GD 形态的收发总入口；普通 DUCAR、CAN IAP、UDS 预留分别位于协议目录。周期发送不是通用表/时间到期框架，而是 GD 样式的一个 5ms 计数器和显式错相分支。APP 通过 E1 把升级请求安全交给 BOOT，BOOT 从首个 CAN TP 页面包头取得升级元数据。

**Tech Stack:** N32G452 REL7、Keil MDK/ArmClang、FreeRTOS CMSIS-RTOS2、CAN 2.0B、J1939 风格 29 位扩展帧和 TP.CM/TP.DT、现有 `d_can` / `app_bms_snapshot` / UART IAP 元数据接口。

**Spec:** [GD7627 同构设计](../specs/2026-09-20-ducar-can-a01-gd7627-direct-port-design.md)

## Global Constraints

- 默认工程根目录始终是 `E:\DUCAR\DUCAR CODE\new platform\DUCAR_N32G45x_Project`；不得在 C 盘工作区修改或编译本任务的工程。
- 结构相似度的判定对象是 `E:\DUCAR\DUCAR CODE\电摩代码整理\7627-GD\02_sw_code\app\trunk\APP\app_can.c` 与 `app_can_xingheng.c`：入口层级、调用顺序、唯一 5ms 计数器、模运算、错相槽位、单帧接收后的 `if / else if` 分流必须逐项可比。
- 不引入 `*_stage`、发送表、到期时间表、`next_ms`、`CAN_TransmitNormalTask()`、`CAN_TransmitPeriodicService()`、`ducar_can_protocol_send_one_periodic()` 或仅转发一层的发送函数。
- 所有新局部变量使用可读匈牙利前缀，例如 `u8*`、`u16*`、`u32*`、`s16*`、`pt*`、`at*`、`au8*`；不使用 `now`、`slot`、`due_slot` 等脱离业务语义的临时变量名。
- 不修改低功耗流程、蓝牙、任务架构、FreeRTOS Port 或 AFE 业务策略。CAN 只调用已有的快照、输入、MOS 和 IAP 边界接口。
- APP CAN 仅使用 `t_can_packet` 作为原始收发帧类型；不得在应用层另建 `_can_stage` 或第二个原始接收队列。
- 未有真实数据源的协议字段必须填 A0.1 定义的无效/保留值，并在发送函数注释中说明；禁止虚构运行数据。F217 半充电次数在 A0.1 表中默认值为 `0x0000`，即使当前没有业务计数器也必须发送 `0x0000`，不能按无效值填 `0xFFFF`。
- A0.1 原表的周期有一条项目覆盖规则：F21A、F220、F221 均在 500ms 槽位发送。F21A/F221 原表的 100ms 标注和 F220 原表的 1000ms 标注不用于本项目主动发送排期。`ducar_can_1000ms_task()` 仍保留为 GD 结构钩子，但当前不发送帧。
- E 盘工程不是 Git 仓库。本计划不包含提交、分支或重置操作；每项完成后只记录实际改动和 Keil 编译结果。

## Review Focus

1. DETD 从“未接入”变为“接入”时，启动一次完整 5 秒窗口（1000 次 5ms）；窗口开始后即使 DETD 再移除也不提前终止，窗口结束后也不会由 DETC、MOS、充放电、预放或 PC 在线状态重新开启。
2. 普通 DUCAR、CAN IAP 和未来 UDS 的边界必须在 `CAN_ReceiveTask()` 第一层清楚可见：标准帧只走 UDS 预留分支，E1/DF00 走 IAP，其他 DUCAR 扩展帧才走普通协议。
3. APP 的 `d_can` 引脚必须与原理图及 BOOT 一致：CAN_RX=PA11、CAN_TX=PA12；不能保留 PB8/PB9 remap2 的旧配置。
4. CAN IAP 的 E1/0x02 不能再把 data[1..4] 当成固件长度；BOOT 只在收到第 0 页 TP 数据并通过品号/长度基础校验后擦 APP2。
5. 任一复位/进入 BOOT 指令必须先发 ACK，等待 CAN 邮箱空闲或触发明确的有限超时，再复位；不可在接收处理函数里直接复位。

---

## Task 1 — 固化原始 CAN 边界、硬件引脚和 GD 形态入口

**Files:**

- Modify: `app\N32G45x_KEIL\User_Src\Driver\d_can.h`
- Modify: `app\N32G45x_KEIL\User_Src\Driver\d_can.c`
- Create: `app\N32G45x_KEIL\User_Src\App\app_can.h`
- Create: `app\N32G45x_KEIL\User_Src\App\app_can.c`
- Modify: `app\N32G45x_KEIL\Core\src\main.c`
- Modify: `app\N32G45x_KEIL\User_Src\Task\comm_task.c`
- Modify: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx`
- Modify: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvoptx` only if Keil needs matching group/view metadata

- [ ] **1.1 建立可编译的 APP CAN 总入口契约。** 在 `app_can.h` 声明以下唯一 APP 层入口，不出现二级转发入口：

  ```c
  bool app_can_init(void);
  void CAN_ReceiveTask(void);
  void CAN_TransmitTask(void);
  ```

  在 `app_can.c` 中建立 `app_can_id_type_t`：`APP_CAN_ID_NONE`、`APP_CAN_ID_UDS`、`APP_CAN_ID_DUCAR_NORMAL`、`APP_CAN_ID_DUCAR_IAP`；声明私有 `app_can_id_check(const t_can_packet *ptCanPacket)`。初始化顺序必须是 `d_can_init(&can0)` → `ducar_can_protocol_init()` → `ducar_can_iap_protocol_init()`。`app_can_init()` 返回底层初始化结果，初始化失败只记录一次明确日志，不让 CAN 失效牵连 FreeRTOS 启动。

- [ ] **1.2 修正 G45x APP 的实际 CAN 引脚。** 在 `d_can.c` 将当前 PB8/PB9、`GPIO_RMP2_CAN1` 的配置改为 PA11 输入上拉（CAN_RX）和 PA12 复用推挽（CAN_TX），开启 GPIOA 时钟，并将 CAN1 两个 remap 均明确关闭。`d_can_deinit()` 对称释放 PA11/PA12。保留现有 RX FIFO ISR、16 帧环形缓存、错误 IRQ 和非排队 `d_can_send()` 行为，不把协议代码放进 driver。

- [ ] **1.3 增加驱动唯一需要的发送完成查询。** 在 `d_can.h/.c` 增加 `bool d_can_is_tx_idle(d_can_t *ptCan);`，只检查 CAN 硬件三个发送邮箱是否均不存在待发送帧。该函数仅为 APP/BOOT 在“ACK 后复位”时确认物理发送完成服务，不建立发送队列、超时表或协议状态机。

- [ ] **1.4 按 GD `app_can_read()` 写接收总入口。** `CAN_ReceiveTask()` 每次只调用一次 `d_can_read()`：空队列立即返回；取到一帧后先调用 `app_can_id_check()`，再用顶层 `if / else if` 清晰分流：

  ```c
  if (eCanIdType == APP_CAN_ID_DUCAR_NORMAL)
  {
      ducar_can_protocol_receive(&tCanPacket);
  }
  else if (eCanIdType == APP_CAN_ID_DUCAR_IAP)
  {
      ducar_can_iap_protocol_receive(&tCanPacket);
  }
  else if (eCanIdType == APP_CAN_ID_UDS)
  {
      /* 标准帧 UDS 入口预留；本轮不调用不存在的 UDS 函数。 */
  }
  ```

  分类规则：标准帧先返回 `APP_CAN_ID_UDS`；扩展帧解码 PF/PS/SA，E1 和归属 DF00 的 TP 帧优先给 IAP；普通扩展帧给 DUCAR；未知帧返回 `NONE`。`SA==0x3A` 只表示 PC 来源，不把所有 PC 帧错误归为普通协议。

- [ ] **1.5 按 GD `app_can_extend_xingheng()` 写发送总入口骨架。** `CAN_TransmitTask()` 只按以下固定顺序调用，不在其中实现字节组包或时间计算：

  ```c
  ducar_can_bus_timer_handle();
  ducar_can_data_renew();
  ducar_can_iap_protocol_task();
  ducar_can_control_reply_task();

  if (ducar_can_detd_tx_window_is_active())
  {
      ducar_can_50ms_task();
      ducar_can_100ms_task();
      ducar_can_200ms_task();
      ducar_can_500ms_task();
      ducar_can_1000ms_task();
  }
  ```

  此处不出现 `CAN_TransmitNormalTask`、周期服务表或任何 `next_ms` 临时变量。

- [ ] **1.6 接入现有初始化和 5ms 通信任务。** 在 `main.c` 保持原有初始化顺序，仅在 UART/IAP 初始化完成、启动调度器之前调用 `app_can_init()`；在 `comm_task.c` 的 5ms 循环中固定按 `CAN_ReceiveTask()` → `CAN_TransmitTask()` → 保留原有 `ducar_iap_protocol_task()` → 心跳 → `osDelay(5U)` 执行。不得新增任务、队列或修改任务优先级。

- [ ] **1.7 同步 Keil 项目目录。** 在 Keil `User_Src/App` 增加 `app_can.c/.h`；在 `User/Protocol/Comm` 和 `User/Protocol/IAP` 预先登记后续实现源文件；保留已存在的 `User/Protocol/UDS` 空目录，不添加伪造 UDS 源。物理目录与 Keil 分组名称完全一致。

- [ ] **1.8 验证。** 编译 APP `N32G45x` 目标，确认新增入口和 PA11/PA12 适配零编译错误；用调试器确认 RX FIFO IRQ 仍进入 `d_can_rx_data_handle()`，并在 CAN 分析仪上确认 APP 上电不因任务接入而主动发送报文。

## Task 2 — 建立普通协议的数据快照、ID 编解码和逐帧组包函数

**Files:**

- Modify: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.h`
- Create: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_types.h`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_bms_snapshot.h` only if the snapshot declaration needs the SOP sub-structure exposed

- [ ] **2.1 删除协议层的第二套原始 CAN 帧概念。** 从 `ducar_can_protocol.h` 移除未使用的 `ducar_can_frame_t`，协议入口一律接收 `const t_can_packet *ptCanPacket`。保留并补全 `ducar_can_id_fields_t`，实现私有的 29 位 ID 编码/解码函数，以及公开的 `bool ducar_can_id_is_pc_source(const ducar_can_id_fields_t *ptCanIdFields);`。所有临时量使用 `u32CanId`、`u8ProtocolFormat`、`ptCanIdFields` 等可读名。

- [ ] **2.2 让 CAN 只读取统一快照。** 在快照中加入只读 SOP 子快照，明确复制当前 `gt_app_sop` 的最大充电电压、最大充电电流、最大放电电流等 CAN 使用值；由 `app_bms_snapshot_update()` 每轮更新。CAN 协议代码不得直接抓取多个业务全局变量，也不得改变 SOP/MOS/故障判断策略。

- [ ] **2.3 建立最小、直接的组包辅助函数。** 在 `ducar_can_protocol.c` 仅保留清楚的 16/32 位小端写入、缩放、饱和和发送 ID 构造辅助函数。辅助函数不能成为通用报文表或函数指针分派；每个 Fxxxx 发送函数必须在自身函数体中可看见快照读取、8 字节含义和 `d_can_send()`。

- [ ] **2.4 一报文一函数实现主动帧。** 创建下列清晰命名的函数：`ducar_can_send_f20d()`、`f20e()`、`f20f()` 至 `f214()`、`f215()`、`f216()`、`f217()`、`f218()`、`f21a()`、`f21b()`、`f21c()`、`f21d()`、`f220()`、`f221()`。字段遵循 A0.1，关键映射固定为：

  - F20D：总压 `.01V`、总流 `.01A`（保留有符号）、SOC `.1%`、SOH、DETC 接入状态。
  - F20E：SOP 允许充/放电电流、最高/最低单体电压。
  - F20F–F214：每帧四个单体电压；第 17–24 串填 `0xFFFF`。
  - F215/F216：单体温度、MOS/最高/最低温度、单体数。
  - F217：循环数、FCC、剩余容量；无现有来源的半充电次数按 A0.1 默认值填 `0x0000`。
  - F218：`mcu_detd.connected`、`mcu_detc.connected`、`mcu_det_load.connected`、当前二次检测输入、CMOS/DMOS/PDMOS、按键在位。并联状态字段固定 A0.1 的“不支持/未并联”编码，不伪造并联 BMS。
  - F21A：SOP 限值；F21B/F21C/F21D：现有故障快照中能明确对应的告警/保护位。未映射位保持协议无故障值并有逐位注释。
  - F220/F221：当前缺少峰值/统计数据源的字段按 A0.1 无效值 `0xFFFF` 或保留值填充，并在函数首部说明数据源缺口。

- [ ] **2.5 固定数据组包验证样本。** 在 CAN 分析仪或调试器中写入已知快照：`pack_mv=76000`、`current_ma=-12340`、`SOC=80`、`SOH=95`、`cell[0]=3300`。DETD 上升沿触发窗口后，捕获 F20D/F20F/F218：核对 F20D 的 `7600`、`-1234`、`800`、`95` 小端字节，核对 F20F 的首单体为 `3300`，以及第 17 串为 `FF FF`。编译 APP，确认无警告升级为错误的新增诊断。

## Task 3 — 原样实现 GD7627 5ms 计数器、错相周期和 DETD 触发窗口

**Files:**

- Modify: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c`
- Modify: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.h`

- [ ] **3.1 建立唯一 5ms 计数器。** 定义 `DUCAR_CAN_CYCLE_50MS=10U`、`100MS=20U`、`200MS=40U`、`500MS=100U`、`1000MS=200U` 与唯一静态 `uint16_t u16Can5msCount`。只有 `ducar_can_50ms_task()` 可自增该计数器，并在 `DUCAR_CAN_CYCLE_1000MS * 2U` 处回绕；其余四个周期函数只读取计数器。

- [ ] **3.2 逐行保留错相 `if / else if` 形式。** 以 GD `app_can_xh_*ms_task()` 为母版实现：50ms 发送 F218；100ms 用 `+1` 至 `+5` 发送 F20D、F20E、F21B、F21C、F21D；200ms 用 `+9` 至 `+14` 发送 F20F–F214；500ms 用 `+15` 至 `+20` 发送 F215、F216、F217、F21A、F220、F221。不得用循环、数组、枚举槽号或消息表替换这些分支。

- [ ] **3.3 保留 1000ms 母版钩子但不偷发报文。** `ducar_can_1000ms_task()` 保留 `+26` 形式与一行“当前项目无 1000ms 主动帧，F220 已按项目规则移至 500ms”的注释；函数不调用 `d_can_send()`。这使以后新增 1000ms 帧时仍可直接按 GD 结构扩展。

- [ ] **3.4 用 DETD 上升沿装载窗口。** 在 `ducar_can_data_renew()` 从本地快照读 `signal.input.mcu_detd.connected`，用上一轮已去抖状态检测 `0 → 1`；仅此时把 `u16DetdProactiveTxTicks` 置为 `1000U`。每次 `ducar_can_detd_tx_window_is_active()` 在该值非零时递减一次并返回真。DETD 移除不清空当前窗口；DETC、PC、MOS、充放电、预放和所有命令不修改该计数器。

- [ ] **3.5 把总线静默与主动窗口分开。** `ducar_can_bus_timer_handle()` 只维护由 PF=E0 明确设置的 CAN 静默/恢复状态；静默期间周期槽位不调用发送函数，但不得重置 DETD 5 秒计数。没有“静默超时自动改业务状态”的隐藏逻辑。

- [ ] **3.6 验证周期和停止条件。** 用 CAN 分析仪从 DETD 去抖后的接入沿开始测量：F218 约 50ms，F20D/F20E/F21B/F21C/F21D 约 100ms，F20F–F214 每 200ms 一条错相帧，F215/F216/F217/F21A/F220/F221 每 500ms 一条错相帧。5 秒后观察到全部主动帧停止；仅改变 DETC、MOS 或 PC 通信状态不能重新出现周期帧；DETD 再次完成移除后接入才启动下一次窗口。

## Task 4 — 普通 DUCAR 接收、按需报文、TP 和控制回复分开落地

**Files:**

- Modify: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.h`
- Modify: `app\N32G45x_KEIL\User_Src\Protocol\Comm\ducar_can_protocol.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_h_power_manage.h`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_h_power_manage.c`
- Modify: `app\N32G45x_KEIL\User_Src\App\app_low_power_manage.h/.c` only if its existing public shutdown API is needed for PF=08 power-off handling

- [ ] **4.1 用 PF 顶层 `switch` 写普通协议接收。** `ducar_can_protocol_receive(const t_can_packet *ptCanPacket)` 先验证扩展帧、DLC 和目的地址，再按已解码 `u8Pf` 写清楚的 `switch`：`EA` 调 `ducar_can_request_receive()`；`EC/EB` 调各自 TP 接收函数；`08/09/0A/0B/0C/E0` 调对应控制函数；默认直接返回。每个分支的注释写“请求”“TP 控制”“TP 数据”“复位/关机”“MOS”“待机电流”“调试”“总线控制”，不混用多层通用分派。

- [ ] **4.2 实现按需报文与长报文 TP。** F200、F20B、F20C、F205、F219、F21E、F21F 可直接应答；F201、F203、F206、F208 必须按 A0.1 RTS/CTS/DT/EOM 发送。TP 状态只保存一个当前发送会话（目标 SA、请求 PGN、总长度、下一个包号、缓冲区），并由 `ducar_can_tp_receive_cts()` / `ducar_can_tp_receive_eom()` 推进；不建立通用队列。RTC F219 调现有 `d_rtc_read()` / `d_rtc_write()`；串号通过 `app_param_get_manufacture_sn()` 获取。没有授权/寿命数据源时按协议 NAK 或无效值返回，不创建假业务状态。

- [ ] **4.3 把控制应答和动作分离。** 接收函数只解析并形成一个命名明确的待回复对象（请求 PF、PS、ACK/NAK、后续动作）；`ducar_can_control_reply_task()` 在发送入口中发送 E8 ACK。复位或关机动作只有在 ACK 已提交且 `d_can_is_tx_idle()` 返回真时执行，并设置有限等待保护，避免邮箱故障造成永久卡住。

- [ ] **4.4 给 MOS 控制增加窄业务入口。** 只在 `app_h_power_manage` 增加能表达 PF=09 的公开接口，例如 `app_h_power_manage_set_remote_mos_command(uint8_t u8MosSelect, app_mos_cmd_t eMosCommand)`。该接口只写 `MF_Remote` 对应的强制命令，合法选择明确为 CMOS、DMOS、PDMOS；未知选择返回失败。CAN 协议不得直接修改 `gt_app_h_power_manage` 内部字段。

- [ ] **4.5 验证接收边界和控制可读性。** 使用 PC `SA=0x3A` 发送：一条标准帧（只进入 UDS 预留、无普通 DUCAR 副作用）、PF=EA 请求 F200（收到对应直接帧）、F201 请求并完成 RTS/CTS/DT/EOM、PF=09 合法 MOS 命令（先看到 E8 ACK，再由 MOS 管理模块在 10ms 周期执行）。发送未知 PF 或非法数据长度必须只得到 NAK/忽略，不污染 TP 会话或周期发送。

## Task 5 — APP CAN IAP：E1 分类、ACK 后切 BOOT 和升级标志复用

**Files:**

- Modify: `app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_uart_iap_protocol.h`
- Modify: `app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_uart_iap_protocol.c`
- Modify: `app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_can_iap_protocol.h`
- Create: `app\N32G45x_KEIL\User_Src\Protocol\IAP\ducar_can_iap_protocol.c`
- Modify: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx`

- [ ] **5.1 只复用 UART IAP 的状态写入，不复制 Flash 逻辑。** 在 UART IAP 模块增加 `bool ducar_iap_prepare_boot_wait_package(void);`：保留现有直接调试防护，复用既有升级状态快照写入路径，将状态写为 `0xBBBBBBBB`。APP CAN IAP 不直接写 Flash 地址、升级状态结构或 APP2。

- [ ] **5.2 明确 APP CAN IAP 的三个命令。** `ducar_can_iap_protocol_receive()` 只处理已由 `app_can_id_check()` 分类的 E1/DF00：E1/0x01 回复“APP 运行区”；E1/0x02 调 5.1 接口并安排 ACK 后复位；E1/0x03 在 APP 侧按协议 NAK，因为完成验证/搬运只能在 BOOT 中执行。DF00 的 TP 只识别升级相关格式，不能落入普通 DUCAR TP。

- [ ] **5.3 用周期任务做 ACK 后复位。** `ducar_can_iap_protocol_task()` 只处理已挂起的 E1/0x02：先经 E8 ACK 发送，确认 `d_can_is_tx_idle(&can0)` 后调用现有 `d_system_reset()`；若硬件邮箱在明确有限时限内一直未空闲，记录 CAN IAP 发送失败并安全复位到 BOOT 等待状态。接收函数内不调用复位。

- [ ] **5.4 验证 APP 到 BOOT 切换。** 发送 `18E1163A`、data[0]=`0x01`，验证 APP 标识 ACK；发送 E1/0x02 且 data[1..7]=`0xFF`，捕获 ACK 后复位，读取升级状态为 `0xBBBBBBBB`，并确认重启后 BOOT 对 E1/0x01 回复 BOOT 标识。验证 E1/0x03 在 APP 侧不错误设置 `0xCCCCCCCC`。

## Task 6 — BOOT CAN IAP 改为 A0.1 的“首页面包头决定元数据”流程

**Files:**

- Modify: `boot\User\Boot\boot_can_upgrade.c`
- Modify: `boot\User\Boot\boot_can_upgrade.h` if the page/session state declaration must同步
- Modify: `boot\Projects\MDK\boot.uvprojx` only if source grouping changes are required

- [ ] **6.1 保持 BOOT 已有 PA11/PA12、500kbps、TP 环形队列和 TX idle 机制。** 仅改 `boot_can_upgrade.c` 的协议状态；不复制 APP 普通协议、不改变 BOOT 内存布局、不改 APP2→APP1 搬运或 Flash 驱动。

- [ ] **6.2 修正 E1/0x02 的入场条件。** 删除对控制帧 DLC≥5 和 `data[1..4]` 固件长度的要求。收到 A0.1 E1/0x02 后仅建立等待首页 TP 会话、保存 PC 源地址并回复 ACK；此阶段禁止擦除 APP2。

- [ ] **6.3 首页面包头成为唯一的元数据来源。** 在 TP RTS/DT 完整拼出地址为 0 的 132 字节页面后，先解析其 128 字节固件头，校验品号、固件长度范围和帧数计算；成功后才写入 `s_boot_can_firmware_size`、计算 `s_boot_can_package_size` 并擦除 APP2。随后只接受按地址递增的 128 字节页面。`boot_can_parse_header()` 不得在赋值前将 `info.size` 与旧的未初始化长度比较。

- [ ] **6.4 保持失败可观察且可恢复。** RTS 长度、包数、PGN、页地址、品号、CRC 或乱序错误都发送 TP Abort，清掉本次会话并回到等待升级状态；不得留下“已擦除但未知道长度”的半初始化状态。E1/0x03 只有在所有页面、固件长度和 CRC 都完成校验时写 `0xCCCCCCCC`，等 CAN TX idle 后复位。

- [ ] **6.5 验证 BOOT 接收时序。** 在 CAN 分析仪发送不携带长度的 E1/0x02，确认 APP2 未被擦除；再发送包含地址 0、128 字节头的 TP RTS/CTS/DT 序列，确认仅在头合法后擦除 APP2；发送一页乱序或错误品号，确认收到 TP Abort 且可重新从 E1/0x02 开始；完整升级后确认 CRC 成功、`0xCCCCCCCC` 写入并走既有 APP2→APP1 启动流程。编译 BOOT `boot` 目标。

## Task 7 — 清理遗留 CAN 文件、核对 GD 同构度并完成双工程验收

**Files:**

- Delete after reference check: `app\N32G45x_KEIL\User_Src\App\app_can_xingheng.h`
- Modify if referenced: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvprojx`
- Modify if referenced: `app\N32G45x_KEIL\MDK-ARM\N32G45x.uvoptx`
- Modify: `tool\tests\pc_source_test.c` only if it remains a maintained test entry; otherwise remove its stale CAN include/reference rather than leave an unusable test

- [ ] **7.1 先做引用核查，再删除遗留物。** 用 `rg` 检查 `app_can_xingheng.h`、`_can_stage`、`CAN_TransmitNormalTask`、`CAN_TransmitPeriodicService`、`ducar_can_protocol_send_one_periodic` 在 APP 源、Keil 工程和测试目录中的引用。只有确认无源码/Keil 引用时，才删除 `app_can_xingheng.h` 和相应项目条目；不得删除 Boot CAN 文件或现有 `d_can`。

- [ ] **7.2 完成可审查的 95% 同构核对。** 逐项列出 GD→G45x 对照：`app_can_init`、`app_can_id_check`、单帧接收 `if / else if`、发送总入口、50/100/200/500/1000ms 五函数、唯一 5ms 计数器、`+1..+5`、`+9..+14`、`+15..+20`、`+26`。每个差异只能属于本计划 Global Constraints 中列出的五类必要差异。

- [ ] **7.3 运行最终编译与硬件验收。** 分别全量编译 APP `N32G45x` 和 BOOT `boot`；记录 Code/RO/RW/ZI 和所有 warning/error。完成一次真实 CAN 验收：上电静默 → DETD 接入 5 秒主动周期帧 → PC 按需/TP 报文 → E1 APP/BOOT 切换 → BOOT TP 首包头升级。最终输出每个 PGN、发送周期、源地址、目标地址、数据来源和无效值原因的审查表。

## Completion Criteria

- APP 以 PA11/PA12 运行 CAN，`d_can` 的 RX 环形缓存仍是唯一原始接收缓冲。
- `CAN_ReceiveTask()` 首层即可看出标准帧 UDS 预留、普通 DUCAR 与 CAN IAP 的去向；`CAN_TransmitTask()` 首层即可看出 IAP、控制回复和五类 GD 周期函数的顺序。
- 主动报文只由 DETD 接入沿开启 5 秒；F21A/F220/F221 在 500ms 槽位。
- 普通协议、TP、控制、CAN IAP 和 BOOT 首包元数据流程均符合 A0.1，不存在旧的“E1 带固件长度”假设。
- APP 与 BOOT Keil 目标均重新编译通过，且 CAN 分析仪验收记录与本计划的周期、边界和升级时序一致。
