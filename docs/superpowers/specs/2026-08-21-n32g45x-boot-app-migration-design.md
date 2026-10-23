# N32G45x BOOT/APP 独立工程迁移设计

## 目标

在当前 `DUCAR_N32G45x_Project` 目录中，参照 `E:\DUCAR\new platform\DUCAR_N32WB4xx_Project` 建立独立的 `boot` 和 `app` 两个 Keil MDK 工程，并将芯片支持迁移到 N32G45x。

## 已确认上下文

- 当前目标目录原本为空，仅包含初始化的 Git 元数据。
- N32WB4XX 模板包含完整的 BOOT、APP、业务模块、板级驱动和 Keil 工程。
- 本机已有 Nations N32G45x 标准外设库 `2.6.0`，包含 CMSIS、启动文件、系统文件和标准外设驱动。
- 模板的 IAP 分区使用 512KB Flash 和 2KB Flash 页：BOOT、APP1、APP2、APP 数据库和 LOG 区的边界已经互相对齐。

## 设计决策

### 芯片和工程目标

- 以 `N32G452REL7` 作为 Keil Device 目标，编译宏使用 `N32G45X, USE_STDPERIPH_DRIVER`。
- 保留 512KB Flash 的现有分区地址和 2KB 页大小。
- 工程目录和输出文件使用 N32G45x 命名；不再依赖模板目录或外部 SDK 相对路径。

### 目录结构

```text
DUCAR_N32G45x_Project/
├── app/
│   ├── Core/
│   ├── Drivers/CMSIS/
│   ├── Drivers/N32G45X_HAL_Drivers/
│   ├── MDK-ARM/N32G45x.uvprojx
│   └── User/
└── boot/
    ├── Core/
    ├── Drivers/CMSIS/
    ├── Drivers/N32G45X_HAL_Drivers/
    ├── Projects/MDK/boot.uvprojx
    ├── RTT/
    └── User/
```

业务、协议、中间件和 RTT 文件从模板复制；芯片 CMSIS、启动文件、标准外设库、APP/BOOT 中的芯片绑定代码和 Keil 工程文件按 N32G45x 重新配置。

### Flash 规划

```text
BOOT       0x08000000 - 0x0800B800
BOOT DB    0x0800B800 - 0x0800C000
APP1       0x0800C000 - 0x0803C000
APP DB     0x0803C000 - 0x08040000
APP2       0x08040000 - 0x08070000
LOG DB     0x08070000 - 0x08080000
```

BOOT 配置、APP Flash map、IAP 地址和 Scatter 文件必须使用同一组边界；若未来芯片型号不是 512KB，需重新设计分区而不是只改 Keil Device 字段。

### 底层适配边界

- `User/App`、协议和中间件尽量保持模板实现不变。
- `User/Driver/*.c`、`User/Board/*`、BOOT `User/Hal_Driver/*` 负责 N32G45x 外设 API、引脚复用、时钟、中断和 Flash 细节。
- 上层头文件不暴露 N32G45x 厂家结构体或 IRQ 类型。
- 本次没有提供新原理图，因此沿用模板的端口、引脚、有效电平和外设映射；上板前仍需按实际 N32G45x 板卡核对。

## 工程配置

- APP 工程：`app/MDK-ARM/N32G45x.uvprojx`，目标名 `N32G45x`，输出 `N32G45x`，使用 APP1 的 `0x0800C000` 链接地址。
- BOOT 工程：`boot/Projects/MDK/boot.uvprojx`，目标名 `boot`，使用 `0x08000000` 链接地址。
- 两个工程均使用相对当前工程目录的源文件路径和 include 路径。
- 调试器、Flash 下载算法和 J-Link 配置以 `N32G452REL7` 的 Keil Device Pack 为准；不复制模板的用户本机生成物、Objects、Listings、日志和 Git 元数据。

## 验证标准

1. 两个 `.uvprojx` 均可被 XML 解析，Device、宏定义、启动文件、系统文件和 include 路径均为 N32G45x。
2. 目标工程的源文件路径全部存在，且不再引用 `N32WB4XX_HAL_Drivers` 或 `n32wb452`。
3. BOOT/APP/APP2/LOG 地址、Flash 页大小和 Scatter 配置一致。
4. 使用 Keil MDK 分别重建 BOOT 和 APP，编译无错误；若出现芯片 API 差异，在对应底层适配层修复。
5. 编译产物包含 BOOT 和 APP 的 HEX，且 APP 的向量表链接地址为 `0x0800C000`。

## 非目标

- 本次不改变 BMS 业务策略、通信协议、IAP 协议格式或产品参数。
- 本次不保证新板卡的实际引脚与外设连线已经验证；硬件验证需要在目标板上完成。
