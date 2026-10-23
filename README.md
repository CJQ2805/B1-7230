# DUCAR N32G45x Project

基于 N32WB4XX 工程迁移的 N32G45x BOOT/APP 项目，目标器件配置为 `N32G452VEL7`（LQFP100），内部 Flash 为 512KB。

## 工程

- APP：`app/MDK-ARM/N32G45x.uvprojx`
- BOOT：`boot/Projects/MDK/boot.uvprojx`
- N32G45x 标准外设库：`app/Drivers/N32G45X_HAL_Drivers`、`boot/Drivers/N32G45X_HAL_Drivers`

## Flash 分区

```text
BOOT    0x08000000 - 0x0800F800
BOOT DB 0x0800F800 - 0x08010000
APP1    0x08010000 - 0x0803F000
APP DB  0x0803F000 - 0x08041000
APP2    0x08041000 - 0x08070000
LOG DB  0x08070000 - 0x08080000
```

APP 已配置 `VECT_TAB_OFFSET=0x00010000`，BOOT/APP 工程均启用 HEX 输出。

## 构建

在 Keil MDK 中分别打开上述两个 `.uvprojx` 并执行 Rebuild。APP 构建后的用户命令会调用 `tool/tool.exe -p n32`，生成升级包和合并生产 HEX。

本次迁移沿用模板中的板级引脚和有效电平；更换实际 N32G45x 板卡前，请按原理图复核 GPIO、UART、AFE 和低功耗唤醒连接。
