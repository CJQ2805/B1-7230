# DUCAR CAN A0.1：GD7627 同构移植设计

## 母版与相似度边界

本次直接以以下文件为母版，不重新设计 CAN 架构：

- `E:\DUCAR\DUCAR CODE\电摩代码整理\7627-GD\02_sw_code\app\trunk\APP\app_can.c`
- `E:\DUCAR\DUCAR CODE\电摩代码整理\7627-GD\02_sw_code\app\trunk\APP\app_can_xingheng.c`

本规格要求 95% 以上的适用代码结构相似度：函数分层、调用顺序、5ms 计数器、模运算、错相槽位、一次取帧后的类别判断、`if / else if` 发送方式均逐项对应母版。

只允许五项必要差异：

1. `hsi_can0_*` 替换为 N32G45x 的 `d_can_*` 与 `t_can_packet`。
2. 并联 BMS、地址分配、1246 充电机、柜机等当前 G45x 不存在的 GD 分支不移植。
3. 星恒 PGN 和字段替换为 DUCAR A0.1 PGN 和字段。
4. 用户指定的业务条件取代 GD 的活跃条件：仅 DETD 的“未接入 → 接入”沿开启 5 秒主动发送窗口。
5. 接收前增加标准帧/扩展帧判断，给后续 UDS 保留入口。

不得引入新的“通用发送表”“到期时间表”“协议发送总服务”“仅转发的 CAN_TransmitNormalTask”或 `_can_stage` 层。

## 函数一一对应

| GD7627 母版 | G45x 目标 | 移植规则 |
| --- | --- | --- |
| `app_can_init()` | `app_can_init()` | 保留入口和初始化顺序，仅将 `hsi_can0_init()` 换为 `d_can_init(&can0)`。 |
| `app_can_id_check()` | `app_can_id_check()` | 保留“返回报文类别枚举”的职责；入参改为 `t_can_packet`。 |
| `app_can_read()` | `CAN_ReceiveTask()` | 保留“取一帧 → ID 分类 → `if / else if` 分发”的形状。 |
| `app_can_extend_xingheng()` | `CAN_TransmitTask()` | 保留总入口的调用顺序和条件分支顺序。 |
| `app_can_xh_50ms_task()` | `ducar_can_50ms_task()` | 保留唯一自增的 5ms 计数器和 `% 10` 判断。 |
| `app_can_xh_100ms_task()` | `ducar_can_100ms_task()` | 保留连续 `if / else if` 和 `+1` 至 `+7` 槽位。 |
| `app_can_xh_200ms_task()` | `ducar_can_200ms_task()` | 保留 `+9` 至 `+14` 槽位。 |
| `app_can_xh_500ms_task()` | `ducar_can_500ms_task()` | 保留连续 `if / else if` 和 `+15` 起始槽位。 |
| `app_can_xh_1000ms_task()` | `ducar_can_1000ms_task()` | 保留 `+26` 槽位。 |
| `app_send_ecan_xxx()` | `ducar_can_send_fxxxx()` | 一个 PGN 一个明确的组包/发送函数，函数内直接调用 `d_can_send()`。 |

## APP 接收：按 `app_can_read()` 结构写

`CAN_ReceiveTask()` 只读取一帧，形态与 GD7627 的 `app_can_read()` 一致：

```c
t_can_packet tCanPacket = {0};
app_can_id_type_t eCanIdType;

if (!d_can_read(&tCanPacket))
{
    return;
}

eCanIdType = app_can_id_check(&tCanPacket);

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
    /* 标准帧 UDS 入口预留；本版本不伪造 UDS 协议处理函数。 */
}
```

`app_can_id_check()` 先看 `u32idType`：标准帧只进入 UDS 预留分类；扩展帧再解码 PF、PS、SA。分类顺序为：CAN IAP（E1/DF00）→ 普通 DUCAR 请求/控制/TP/报文 → UDS 预留 → 未知帧。PC 源地址 `SA == 0x3A` 只用于识别 PC 来源。

普通 DUCAR 内部继续按 GD 同类模式分流：请求、控制、TP 三类各有独立处理函数。以后加入 UDS 或宗申协议，只在 `app_can_id_check()` 添加类别与调用分支，不改普通 DUCAR 函数。

## APP 发送：按 `app_can_extend_xingheng()` 结构写

`CAN_TransmitTask()` 固定为：

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

GD7627 原先在主条件成立时调用 50/100/1000ms，在 PC 在线时调用 200/500ms。本项目按用户明确要求调整为：DETD 接入沿装载 1000 个 5ms 计数；5 秒内所有周期任务都运行；窗口结束后全部主动报文停止。DETC、MOS、充放电、预放、PC 在线都不能重新开启窗口。

不存在 `CAN_TransmitNormalTask()`，也不存在只做转发的 `ducar_can_protocol_transmit_task()`。

## 5ms 计数器和槽位

代码形态直接沿用母版：

```c
#define DUCAR_CAN_CYCLE_50MS    10U
#define DUCAR_CAN_CYCLE_100MS   20U
#define DUCAR_CAN_CYCLE_200MS   40U
#define DUCAR_CAN_CYCLE_500MS   100U
#define DUCAR_CAN_CYCLE_1000MS  200U

static uint16_t u16Can5msCount;
```

只有 `ducar_can_50ms_task()` 执行 `++u16Can5msCount`，并在达到 `DUCAR_CAN_CYCLE_1000MS * 2U` 时清零。其他函数只读取这个计数器，不创建 `next_ms`、报文表、槽号变量或时间到期状态机。

| 周期 | 槽位写法 | DUCAR A0.1 报文 |
| --- | --- | --- |
| 50ms | `u16Can5msCount % 10U == 0U` | F218 |
| 100ms | `+1` 至 `+5`，连续 `else if` | F20D、F20E、F21B、F21C、F21D |
| 200ms | `+9` 至 `+14`，连续 `else if` | F20F、F210、F211、F212、F213、F214 |
| 500ms | `+15` 至 `+20`，连续 `else if` | F215、F216、F217、F21A、F220、F221 |
| 1000ms | `+26` | 保留 GD7627 同构函数和槽位；本版本没有主动报文放在此槽位 |

### 项目排期覆盖规则

协议 A0.1 原始表中 F21A/F221 标为 100ms、F220 标为 1000ms；本项目按用户明确决定覆盖为 500ms。该覆盖只改变发送时隙，不改变三条报文的 ID、字节定义、请求处理或字段缩放。`ducar_can_1000ms_task()` 仍按 GD7627 的函数层级被调用，并保留 `+26` 槽位注释，以便后续新增 1000ms 主动报文时不破坏母版结构；当前该函数不发送任何帧。

例如 200ms 函数的结构必须保持：

```c
if (((u16Can5msCount + 9U) % DUCAR_CAN_CYCLE_200MS) == 0U)
{
    ducar_can_send_f20f();
}
else if (((u16Can5msCount + 10U) % DUCAR_CAN_CYCLE_200MS) == 0U)
{
    ducar_can_send_f210();
}
```

依此写完至 F214；不替换为循环、数组或表驱动。

## A0.1 数据映射

- F20D/F20E/F21B/F21C/F21D：100ms。
- F20F–F214：200ms 的 24 串单体电压。当前快照只有 16 串，17–24 串固定填 `0xFFFF`，不填 0mV。
- F215/F216/F217/F21A/F220/F221：500ms 温度、MOS 温度、循环次数、FCC、容量、SOP 限值和统计/保护字段。
- F217 的半充电次数当前没有业务数据源；A0.1 表定义的默认值为 `0x0000`，因此发送 `0x0000`，不以通用无效值 `0xFFFF` 替代。
- F218：50ms 数字输入和 MOS 状态。
- F200/F201/F203/F205/F206/F208/F20B/F20C/F219/F21E/F21F：普通协议的请求和 TP 分支。
- F800/F900/FA00/FB00/FC00/FE000：普通协议的控制分支，按 A0.1 ACK 格式回复。

每个 `ducar_can_send_fxxxx()` 自己完成快照读取、8 字节组包、29 位扩展 ID 和 `d_can_send()` 调用。没有数据源的字段填协议无效值或保留值，不伪造业务数据。

## CAN IAP 与 BOOT

CAN IAP 不进入普通 DUCAR 接收函数：E1/DF00 分类后直接交 `ducar_can_iap_protocol_receive()`。

1. APP 收到 PC 的 E1/0x01，回复 APP 运行区标识。
2. APP 收到 E1/0x02，通过 UART IAP 模块提供的窄接口写 `0xBBBBBBBB`，发送 ACK 后复位进入 BOOT。
3. BOOT 处于等待 CAN 包状态时，E1/0x01 回复 BOOT 标识。
4. A0.1 的 E1/0x02 不带固件长度；BOOT 必须删除当前对 data[1..4] 的长度要求，改由 TP RTS 和首个 128 字节包头取得长度、CRC、品号。
5. APP2 接收、CRC、`0xCCCCCCCC` 标志和 APP2 → APP1 搬运继续在 `boot_can_upgrade.c`，不复制到 APP 普通协议。

## 文件归属

- `User_Src/App/app_can.c/.h`：`app_can_init()`、`app_can_id_check()`、`CAN_ReceiveTask()`、`CAN_TransmitTask()`；对应 GD `app_can.c`。
- `User_Src/Protocol/Comm/ducar_can_protocol.c/.h`：`ducar_can_data_init/renew()`、五个周期函数、全部 Fxxxx 发送函数、普通 DUCAR 接收；对应 GD `app_can_xingheng.c`。
- `User_Src/Protocol/IAP/ducar_can_iap_protocol.c/.h`：APP CAN IAP 入口和切入 BOOT。
- `User_Src/Protocol/UDS/`：只建目录，不写伪造 UDS。
- `Driver/d_can.c/.h`：只保留 ISR、环形缓存、收发硬件职责。
- `Task/comm_task.c`：每 5ms 依次调用 `CAN_ReceiveTask()`、`CAN_TransmitTask()`，保留 UART IAP 任务。

## 验收

1. 用 GD 源码逐项核对：函数层级、调用顺序、计数器唯一递增点、槽位偏移、`if / else if` 链一致。
2. DETD 接入后验证 5 秒内全部 A0.1 主动周期报文发送；窗口结束后停止。
3. 验证 PC `SA=0x3A` 请求、TP、CAN IAP 不会被普通报文函数消费。
4. 完整重新编译 APP 和 BOOT Keil 目标。
