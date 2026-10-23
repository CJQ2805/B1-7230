# BMS 配置清单

目标芯片：N32G452REL7

配置入口：`app/User/App/app_bms_config.h`

## 配置归属

| 文件 | 配置内容 |
|---|---|
| `app_bms_config.h` | BMS 保护总开关和保护子开关 |
| `app_bms_fault_config.h` | 保护阈值、恢复阈值、动作延时 |
| `app_balance_manage.h` | 均衡开关、均衡阈值和均衡接口 |
| `app_param.h` | 产品身份参数、电流零点死区等整机参数 |
| `FreeRTOSConfig.h` | RTOS 内核参数 |

## BMS 保护开关当前值

| 开关 | 当前值 |
|---|---:|
| `BMS_FEATURE_FAULT_PROTECT_ENABLE` | `1U` |
| `BMS_PROTECT_AFE_FAULT_ENABLE` | `1U` |
| `BMS_PROTECT_NTC_FAULT_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_OV_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_OV_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_OV2_PRO_ENABLE` | `0U` |
| `BMS_PROTECT_PACK_OV_WARN_ENABLE` | `0U` |
| `BMS_PROTECT_PACK_OV_PRO_ENABLE` | `0U` |
| `BMS_PROTECT_CELL_UV_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_UV_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_DEEP_UV_ENABLE` | `0U` |
| `BMS_PROTECT_PACK_UV_WARN_ENABLE` | `0U` |
| `BMS_PROTECT_PACK_UV_PRO_ENABLE` | `0U` |
| `BMS_PROTECT_CHG_OT_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_CHG_OT_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_CHG_UT_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_CHG_UT_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_OT_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_OT_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_UT_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_UT_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_MOS_OT_WARN_ENABLE` | `1U` |
| `BMS_PROTECT_MOS_OT_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_CHG_OC_WARN_ENABLE` | `0U` |
| `BMS_PROTECT_CHG_OC_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_CHG_OC_LOCK_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_OC_WARN_ENABLE` | `0U` |
| `BMS_PROTECT_DSG_OC_PRO_ENABLE` | `1U` |
| `BMS_PROTECT_DSG_OC_LOCK_ENABLE` | `1U` |
| `BMS_PROTECT_SHORT_ENABLE` | `0U` |
| `BMS_LOAD_DETECT_ENABLE` | `1U` |
| `BMS_PROTECT_CELL_DIFF_WARN_ENABLE` | `0U` |
| `BMS_PROTECT_CELL_DIFF_PRO_ENABLE` | `0U` |

兼容宏 `BMS_SW_PROTECT_ENABLE` 由 `BMS_FEATURE_FAULT_PROTECT_ENABLE` 提供，不再单独配置。

## 均衡配置当前值

均衡配置保留在已有 `app_balance_manage.h`：

| 配置项 | 当前值 |
|---|---:|
| `APP_BALANCE_ENABLE` | `1U` |
| `APP_BALANCE_START_DIFF_MV` | `30U` |
| `APP_BALANCE_STOP_DIFF_MV` | `10U` |
| `APP_BALANCE_MIN_CELL_MV` | `3200U` |

均衡关闭时，软件必须清除 AFE 均衡位图。

## 固定启用功能

以下功能不提供顶层开关，始终保留在初始化和任务路径：

- MOS 高压服务；
- 低功耗管理；
- 普通 BMS 通信；
- IAP 通信；
- 电流校准；
- 故障/运行日志；
- MCU NTC 采样；
- LED 状态指示；
- SOC 计算。

## 待另行审核的注释矛盾

本次迁移保持宏值不变，但以下注释与当前宏值存在差异，需结合硬件规格书另行确认：

- `COV_SET_WAR_VOLTAGE` 当前为 `3300U`，注释写成 `4220mV`；
- `COV_RECOVER_WAR_VOLTAGE` 当前为 `3150U`，注释写成 `4100mV`；
- `COV_SET_PRO_VOLTAGE` 当前为 `3350U`，注释写成 `4235mV`；
- `CUV_SET_WAR_VOLTAGE` 当前为 `2250U`，注释写成 `2850mV`；
- `CUV_RECOVER_WAR_VOLTAGE` 当前为 `2400U`，注释写成 `3100mV`；
- `DCUV_SET_PRO_VOLTAGE` 当前为 `1500U`，注释写成 `2000mV`；
- 多个充放电温度恢复/保护注释与宏值也需要按规格书复核。

这些差异不属于本次“开关归属整理”，未在本次修改中自动修正。
