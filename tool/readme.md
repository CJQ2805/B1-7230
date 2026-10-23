# DUCAR 串口升级包工具

## Keil 配置

APP 工程 `Options for Target...` -> `Output` 需要勾选 `Create HEX File`。

APP 工程 `Options for Target...` -> `User`：

Run #1：

```text
fromelf.exe  --bin --output=.\Objects\@L.bin !L
```

Run #2：

```text
..\..\..\tool\tool.exe -p n32
```

BOOT 工程也需要勾选 `Create HEX File`，这样每次重新编译 BOOT 后都会生成最新的：

```text
boot\Projects\MDK\Objects\boot.hex
```

## 串口升级包

工具读取 `tool\config.json` 中的 `n32` 配置，默认从 APP 参数头文件读取品号、型号、T版本和V版本：

```text
app\N32G45x_KEIL\User_Src\App\app_param.h
```

默认读取 APP 原始 BIN：

```text
app\N32G45x_KEIL\MDK-ARM\Objects\N32G45x.bin
```

输出：

```text
tool\<APP_PARAM_SOFTWARE_PN>_<APP_PARAM_BAT_TYPE>_<APP_PARAM_PACKAGE_T_VERSION>_<APP_PARAM_PACKAGE_V_VERSION>.bin
tool\log.txt
```

升级包 `.bin` 格式：

- 第 0 包：128 字节升级头，包含 `DUCAR FF FF`、10 字节品号、APP CRC32、APP 原始长度。
- 后续包：APP 原始 bin 数据。
- 最后一包不足 128 字节时，用 `0xFF` 补齐。

## 生产烧录 HEX

默认配置会额外生成生产烧录用 HEX，命名规则和升级 BIN 使用同一个基础名：

```text
tool\<APP_PARAM_SOFTWARE_PN>_<APP_PARAM_BAT_TYPE>_<APP_PARAM_PACKAGE_T_VERSION>_<APP_PARAM_PACKAGE_V_VERSION>.hex
```

生成规则：

- 每次运行工具时，先把 `boot\Projects\MDK\Objects\boot.hex` 复制到 `tool\boot.hex`。
- 读取 `tool\boot.hex`，要求 BOOT 地址必须落在 `0x08000000~0x0800F7FF`。
- 读取 `app\N32G45x_KEIL\MDK-ARM\Objects\N32G45x.hex`，要求 APP 地址必须落在 `0x08010000~0x0803EFFF`。
- 不在 `0x0800FFFC` 写入升级标志，保持 BOOT 数据页擦除态；BOOT 读到 `0xFFFFFFFF` 时按默认上电流程跳转 APP1，APP 真正运行后再写入 `0xAAAAAAAA`。
- 合成与升级 BIN 同名的 `.hex` 文件。

注意：APP 工程必须链接到 `0x08010000`，并且宏定义必须包含：

```text
VECT_TAB_OFFSET=0x00010000
```

如果 APP 还链接在 `0x08000000`，工具会认为 APP HEX 地址超出 APP1 允许范围并报错。

## 临时出包

也可以像 GD 工具一样临时覆盖参数：

```text
tool.exe -p n32 -bin app.bin -out output_base -s2 A123456789 -n DUCAR -t 13 -v 101
```

这种指定 `-bin` 的临时出包只生成串口升级包，不自动生成生产烧录 HEX。
