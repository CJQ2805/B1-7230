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

## ZCANPRO UDS 升级

运行以下脚本会依次编译 BOOT 和 APP、生成 DUCAR APP BIN、从 128 字节头包后提取裸 APP1 S19，并同步更新 `tool\boot.hex`：

```text
tool\build_uds_upgrade_package.bat
```

脚本默认使用 `E:\Keil_v5\UV4\uVision.com` 和当前用户的 Codex Python 运行时；也可以在运行前设置 `UVISION`、`PYTHON` 指向本机安装路径。完成后导入：

```text
tool\ZS_ECU_Flash_20250401.zflash
```

该 ZCANPRO 流程引用同目录的 `SeednKey.dll`、`BMS-DUCAR_FlashDriver.s19` 和 `A800000002_7230B1_V01_T12_APP1.s19`。APP S19 的数据从 `0x08010000` 开始，内容是 BIN 去掉 128 字节 DUCAR 头后的 APP；BOOT 直接写 APP1，并校验 APP CRC16。
`SeednKey.dll` 是客户提供的 ZCANPRO 本地依赖，请放在 `tool` 同目录；源代码提交不包含该 DLL。

构建脚本遇到任一 Keil 编译错误会停止，不会重新打包旧的 APP 输出文件。

构建脚本开始时会先删除上次生成的 APP1 S19，避免链接失败后仍误导入旧升级镜像。

当前目录此前生成的 `A800000002_7230B1_V01_T12_APP1_FROM_OLD_BIN.s19` 是从旧 T12 BIN 提取的，仅用于保留旧产物；它不包含本次安全访问修改，不能用于验证本次升级。需在 BOOT 与 APP 均成功链接、重新生成 BIN 后，再运行上述脚本生成可导入的最新 APP1 S19。

## 临时出包

也可以像 GD 工具一样临时覆盖参数：

```text
tool.exe -p n32 -bin app.bin -out output_base -s2 A123456789 -n DUCAR -t 13 -v 101
```

这种指定 `-bin` 的临时出包只生成串口升级包，不自动生成生产烧录 HEX。
