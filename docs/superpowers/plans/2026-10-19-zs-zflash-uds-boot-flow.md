# 宗申 `.zflash` UDS/BOOT 兼容实施计划

> **执行说明：** 计划获批后由当前执行者在现有工作区逐项实施；当前工作区已有的其他改动必须保留。

**目标：** 让 APP 与 BOOT 按已确认的宗申 `.zflash` 流程完成 UDS 升级，并保留现有 DUCAR UART/CAN 升级流程。

**架构：** 正常 APP 升级仍由 APP 处理预编程阶段，并在物理 `10 02` 肯定响应发送完成后交接到 BOOT。若升级中断且设备留在 BOOT 的持久 `WAIT_PACKAGE` 状态，BOOT 也能从 `.zflash` 预编程开头重新开始，并在 BOOT 内切换到编程会话；APP 交接后直接从编程阶段开始的既有路径保留。正常 APP 有效启动时 BOOT 仍立即跳转，不增加监听窗口。BOOT 复用现有 UDS 传输/服务框架，增加 BOOT CAN 适配和下载后端；强制验证完整 9 字节 `BMS-DUCAR` 标记及传输 CRC，BOOT 内接收的应用数据直接擦写 APP1，完整校验后以持久 `APP1_READY` 状态交接并在复位后再次核验。APP 正常运行时的自升级和原有 DUCAR CAN/UART 升级仍使用 APP2 暂存/搬运路径。下载数据不作为代码执行；Flash 操作由 BOOT 内部 Flash API 完成。

**技术栈：** C、N32G45x、Keil MDK-ARM、标准 11-bit CAN、ISO-TP/UDS、内部 Flash。

**设计依据：** `docs/superpowers/specs/2026-10-19-zs-zflash-uds-boot-flow.md`

## 全局约束

- UDS 物理请求/响应/功能地址固定为 `0x7A0`、`0x7A8`、`0x7DF`。
- APP1 逻辑代码区为 `0x08010000..0x0803EFFF`；APP2 暂存区为 `0x08041000..0x0806FFFF`；写入前必须检查地址、长度和溢出。
- 第一个下载项必须完整等于 ASCII `BMS-DUCAR` 九字节并通过 CCITT-FALSE CRC；缺失或不匹配时停止升级。
- 下载到 RAM 的数据不执行；FlashDriver 下载项按标记块校验，擦除/写入使用 BOOT 自身接口。
- APP/BOOT 现有 DUCAR UART/CAN 升级行为保持兼容；BOOT 内 UDS 升级直接写 APP1；不改 AFE 电芯拓扑方案。
- 当前缺少 `FlsDrv.s19` 与 `rt5_crc32_release.s19`，实施时按批准设计实现地址边界，但不得声称已用真实 S-record 验证地址分段。
- 不新增或运行自动化测试；完成后做源码审查，并在 Keil 工具可用时做 APP/BOOT 编译检查。

## 重点审查场景

- `0x7DF` 功能寻址的静默命令不能产生肯定响应；`0x7A0` 物理请求必须从 `0x7A8` 响应。
- `10 02` 的肯定响应必须先完成发送，随后才能复位；复位后 BOOT 必须以编程会话接收下一条请求。
- 缺少/错误/重复的 `BMS-DUCAR` 标记及不匹配的 CRC 均不能允许 APP 擦写或启动。
- `28 83 03` 仅允许扩展会话且一级解锁后执行；抑制普通通信的期限至少为 300 秒，不因后续 UDS 请求续期；RTC 完整日历秒数按 301 秒设置，STOP 休眠也计入。RTC 回拨时重新设置差分基准，不能误算成跨午夜而提前恢复。
- `34/36/37` 的序号、重复块、长度及 `31 FF00` 地址边界不能导致 APP1 越界写入或擦除。
- `31 FF01` 只在标记、指纹、镜像数据和 CRC 均通过后准备 `APP1_READY`；异常流程不得提交启动状态。
- BOOT 验证 APP1 并复位后，APP 的默认会话必须接受 `28 80 03`、`85 81`，并恢复普通通信。

---

### Task 1：APP 预编程和 BOOT 交接

**文件：**
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSBase/uds_port.h`
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSBase/uds_port.c`
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/service_cfg.c`
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID10_SessionControl.c`
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID28_CommunicationControl.c`
- 修改：`app/N32G45x_KEIL/User_Src/App/app_low_power_manage.c`
- 修改：`app/N32G45x_KEIL/User_Src/Driver/d_rtc.h/.c`
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID85_ControlDTCSetting.c`
- 视需要修改：`app/N32G45x_KEIL/User_Src/Protocol/IAP/ducar_uart_iap_protocol.c/.h`

**接口：** APP 继续使用 `uds_recv_frame()`、`uds_send_frame()`；增加固定宗申地址及功能寻址接收分类；编程会话切换调用现有 `ducar_iap_prepare_boot_wait_package()`，发送完成后由 `uds_request_reset_after_tx()` 复位。

- [ ] **步骤 1：** 将 APP UDS 物理请求/响应地址改为 `0x7A0/0x7A8`，识别 `0x7DF` 功能地址并标记为功能寻址，不改变普通扩展 CAN 的分流。
- [ ] **步骤 2：** 按 `.zflash` 允许扩展会话下功能寻址的 `85 82`、`28 83 03`，并允许默认会话下恢复命令 `28 80 03`、`85 81`；两类服务按请求抑制肯定响应。
- [ ] **步骤 2a：** `28 83 03` 仅限扩展会话和一级解锁，设置至少 300 秒固定期限（按 301 秒计时），不因 UDS 活动续期；使用 RTC 完整日历秒数累计 STOP 休眠时间，并在 RTC 周期唤醒路径检查到期。RTC 回拨时重设差分基准，不误判为午夜；RTC 不可读时拒绝开启抑制，运行中不可读则恢复普通通信。
- [ ] **步骤 3：** 物理 `10 02` 仅在扩展会话和一级解锁后受理；拒绝抑制肯定响应的 `10 82`，准备 BOOT 等待标志、提交 `50 02` 肯定响应，并只在发送完成后复位；不把功能 `10 83` 误当成 BOOT 交接。重新请求当前扩展会话时保留已取得的一级解锁。
- [ ] **步骤 4：** 审查 APP 现有 F18A/F193/F189、一级安全访问与静默状态的实现，修正发现的协议差异。

### Task 2：BOOT 标准 CAN 与 UDS 生命周期

**文件：**
- 修改：`boot/User/Hal_Driver/boot_can.c`
- 修改：`boot/User/Hal_Driver/boot_can.h`
- 修改：`boot/User/Boot/boot_manage.c`
- 修改：`boot/Core/Src/main.c`
- 修改：`boot/Projects/MDK/boot.uvprojx`
- 新建：`boot/User/Boot/boot_uds.c/.h`
- 修改：`boot/User/UDS/UDSLogic/SID10_SessionControl.c`
- 修改：`boot/User/UDS/UDSLogic/SID22_ReadDataByIdentifier.c`
- 修改：`boot/User/UDS/UDSLogic/service_cfg.c/.h`
- 修改：`boot/User/UDS/UDSLogic/boot_uds_support.c` 和 BOOT 的 SID28/SID85 头文件

**接口：** `boot_can_send_frame()` 支持标准 CAN 数据帧；BOOT UDS 接收入口消费 `0x7A0` 物理请求；BOOT 进入 `WAIT_PACKAGE` 时将 UDS 会话设为编程会话，按 1 ms 推进传输和服务定时器。

- [ ] **步骤 1：** 扩展 BOOT CAN 发送队列的标准帧路径，保持扩展帧的 DUCAR ID 校验和发送逻辑不变。
- [ ] **步骤 2：** 在 BOOT CAN 接收循环中将标准 `0x7A0`/`0x7DF` 分派到 UDS，继续把扩展帧分派给原有 DUCAR 升级状态机。
- [x] **步骤 3：** 仅当 BOOT 升级标志为 `WAIT_PACKAGE` 时启动 UDS；初始编程会话继续兼容 APP 交接后的直接下载，且允许 `.zflash` 从扩展会话预编程阶段重新开始。BOOT 上电正常跳 APP 的路径不启用 UDS 下载服务。
- [ ] **步骤 4：** 在 BOOT 主循环基于 `boot_timer_get_ms()` 提供 1 ms UDS 周期推进，并处理 UDS 响应发送完成后的复位请求。
- [ ] **步骤 5：** 将所需的 UDS TP/服务源文件加入 BOOT Keil 工程，并设置 BOOT 适配宏；APP/BOOT 共用服务逻辑，平台差异留在端口和升级后端。
- [x] **步骤 6：** `WAIT_PACKAGE` 是掉电保持的恢复态。BOOT 重启后清空 RAM 中未完成的 UDS 传输状态并继续等待；不能因 APP1 存在部分旧数据而直接启动。通过预编程入口重启一轮时，重新接收并校验整套镜像。

### Task 3：BOOT 侧宗申服务和升级状态机

**文件：**
- 新建：`boot/User/Boot/boot_uds_upgrade.c/.h`
- 新建：BOOT 专用 UDS 服务注册源/头文件
- 修改：`boot/User/Boot/boot_config.h`
- 修改：`boot/User/Boot/boot_upgrade.c/.h`（仅在现有接口不足时）
- 修改：`boot/Projects/MDK/boot.uvprojx`
- 复用：APP UDS 的 `uds_tp`、`uds_service`、`SID10/11/27/2E/31/34/36/37/3E` 和 `uds_customer_profile` 公共逻辑

**接口：** BOOT 后端实现 `uds_upgrade.h` 定义的会话 API，包括 `uds_upgrade_request_download()`、`uds_upgrade_transfer_data()`、`uds_upgrade_transfer_exit()`、CRC 校验、擦除、依赖检查和准备 APP1 启动。

- [x] **步骤 1：** 建立 BOOT UDS 服务注册表：保留 APP 交接后的直接编程会话路径；在 `WAIT_PACKAGE` 恢复入口另外支持 `.zflash` 预编程阶段所需的扩展会话、安全访问、`F18A/F193/F189` 版本 DID、`85 82` 和 `28 83 03`。供应商名及 V/T 版本从 APP 的 `app_param.h` 读取；BOOT 中 `28/85` 按 APP 的会话、安全和响应抑制规则处理，但不操作未运行的 APP 普通通信/DTC记录。在 BOOT 收到物理 `10 02` 后只切换到编程会话，不复位；功能寻址预编程命令按协议抑制肯定响应。正常启动路径不启用该入口。
- [ ] **步骤 2：** 在 BOOT 收到第一个下载项时要求总数据长度为 9 字节，逐字节匹配 `BMS-DUCAR`；缺失、内容不匹配或 CRC 不一致都拒绝流程。
- [ ] **步骤 3：** 对每个下载传输维护独立的 CCITT-FALSE CRC16；`36` 的 BSC 不参与 CRC，支持正确序号和同内容重复块，不接受非法长度或错序块。
- [ ] **步骤 4：** `F184` 校验年月日 BCD 与 4 字节 ASCII 设备号，允许 `.zflash` 中的 `00 00 00 00`；指纹写入既有 APP 数据库专用记录区。
- [ ] **步骤 5：** 实现显式 `31 FF00`：RAM 标记块按安全 RAM 范围校验并不擦 Flash；APP1 直接擦除，限制在代码分区并检查长度溢出。
- [x] **步骤 6：** 实现 `34/36/37` 的 APP 镜像接收，按逻辑 APP1 地址直接写入；已发 `FF00` 时校验授权范围；因本 `.zflash` 未列 `FF00`，直接接受合法 APP `34` 后按页惰性擦除再写入。
- [x] **步骤 7：** `31 0202` 对当前传输数据做 CRC16 比对；`31 FF01` 仅在必需标记、指纹、镜像长度/CRC 都有效时写入带长度/CRC 的 `APP1_READY` 状态。
- [x] **步骤 8：** `11 03` 返回 `51 03` 后等待 CAN TX 空闲并复位；BOOT 复位后核验 APP1 向量表及整包 CRC32，通过后清除升级元数据、写入 `APP_VALID` 并启动 APP1。若核验失败，回到掉电保持的 `WAIT_PACKAGE` 接收重刷。

### Task 4：安全访问、复位后恢复与集成审查

**文件：**
- 修改：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/uds_customer_profile.h/.c`
- 修改：BOOT UDS 端口与服务注册文件
- 复核：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID27_SecurityAccess.c`
- 复核：`app/N32G45x_KEIL/User_Src/Protocol/UDS/UDSLogic/SID22_ReadDataByIdentifier.c`
- 复核：APP/BOOT 两个 `*.uvprojx` 工程文件

- [x] **步骤 1：** 仅静态检查 `SeednKey_20250401.dll`；确认它是 32 位 PE 且导出 `GenerateKeyEx`，未执行 DLL。当前环境缺少反汇编器，未能确认 KeyConst。
- [x] **步骤 2：** 未确认常量时移除全零猜测值，APP/BOOT 对 Key 请求返回 NRC `0x22`；`27 01/02`、`27 09/0A` 不标为已兼容，并在设计稿中记录未解决项。
- [ ] **步骤 3：** 复核 APP 默认会话恢复命令的服务许可和通信开关；确认 BOOT 验证 APP1 并复位后，APP 发出普通通信并能无响应处理 `28 80 03`、`85 81`、`10 01`。
- [ ] **步骤 4：** 对照实施计划审查修改差异，确认 DUCAR UART/CAN 入口未被改写、Flash 地址严格限定、下载代码没有执行路径。
- [ ] **步骤 5：** 如 Keil MDK 构建工具可用，分别编译 APP 与 BOOT 工程；本计划不新增或运行自动化测试。

## 完成时的限制说明

由于未取得 `FlsDrv.s19` 与 `rt5_crc32_release.s19`，只能确认源码中的服务顺序、标记校验和地址保护逻辑；不能声称已经核对真实 S-record 地址段或完成台架刷写验证。
