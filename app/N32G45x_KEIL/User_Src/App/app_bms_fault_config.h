#ifndef __APP_BMS_FAULT_CONFIG_H_
#define __APP_BMS_FAULT_CONFIG_H_

#include "Driver/d_timer.h"

/*
 * BMS故障配置集中管理。
 *
 * 这个文件集中放置“故障/告警判断需要用户配置的参数”，包括保护开关、阈值和延时。
 * 不放整机身份参数，也不放底层采样修正参数。
 *
 * 配置规则：
 * 1. 所有 *_ENABLE 宏：1U表示启用，0U表示关闭。
 * 2. 电压阈值单位：mV。
 * 3. 普通电流阈值单位为10mA；MOS失效电流阈值单位为mA。正数表示充电方向，负数表示放电方向。
 * 4. 温度阈值单位：摄氏度。
 * 5. 延时必须使用TMR0_DELAY_MS/SEC/MIN转换，保证和系统1ms时基一致。
 *
 * 注意：
 * 电流零点死区不在这里配置。AFE换算出mA电流后，会先按app_param.h里的
 * APP_PARAM_CURRENT_ZERO_DEADBAND_MA 做归零处理，然后保护模块只使用已经处理后的电流。
 */

/*
 * BMS保护开关配置区。
 *
 * 每一个 *_ENABLE 宏只负责一个明确的故障检测或保护功能，开关之间不合并。
 * 1U表示编译并运行对应功能，0U表示编译时裁剪对应功能。
 * 保护阈值、恢复阈值和动作延时继续放在本文件下面；均衡配置仍放在
 * app_balance_manage.h。
 */

/*
 * 软件保护总开关。
 *
 * 开启后，APP层会执行电压、温度、电流、压差和NTC等软件阈值判断。
 * 关闭后，所有软件保护子开关会在下面的配置分支中强制置0U，软件阈值判断
 * 不再运行；AFE已经产生的硬件保护状态仍由AFE故障链路负责处理。
 */
#define BMS_FEATURE_FAULT_PROTECT_ENABLE       1U

/*
 * 软件保护兼容开关。
 *
 * 该宏保留给历史代码和旧接口使用，实际值始终跟随BMS_FEATURE_FAULT_PROTECT_ENABLE，
 * 不应单独修改，也不代表某一种具体保护类型。
 */
#define BMS_SW_PROTECT_ENABLE                  BMS_FEATURE_FAULT_PROTECT_ENABLE

#if (BMS_FEATURE_FAULT_PROTECT_ENABLE == 0U)
/* 主保护开关关闭时，AFE通信/数据有效性软件保护强制关闭。 */
#define BMS_PROTECT_AFE_FAULT_ENABLE           0U

/* 主保护开关关闭时，NTC异常软件保护强制关闭。 */
#define BMS_PROTECT_NTC_FAULT_ENABLE           0U

/* 主保护开关关闭时，单体过压告警强制关闭。 */
#define BMS_PROTECT_CELL_OV_WARN_ENABLE        0U

/* 主保护开关关闭时，单体一级过压保护强制关闭。 */
#define BMS_PROTECT_CELL_OV_PRO_ENABLE         0U

/* 主保护开关关闭时，单体二级过压保护强制关闭。 */
#define BMS_PROTECT_CELL_OV2_PRO_ENABLE        0U

/* 主保护开关关闭时，总压过压告警强制关闭。 */
#define BMS_PROTECT_PACK_OV_WARN_ENABLE        0U

/* 主保护开关关闭时，总压过压保护强制关闭。 */
#define BMS_PROTECT_PACK_OV_PRO_ENABLE         0U

/* 主保护开关关闭时，单体欠压告警强制关闭。 */
#define BMS_PROTECT_CELL_UV_WARN_ENABLE        0U

/* 主保护开关关闭时，单体一级欠压保护强制关闭。 */
#define BMS_PROTECT_CELL_UV1_PRO_ENABLE        0U

/* 主保护开关关闭时，单体二级欠压保护强制关闭。 */
#define BMS_PROTECT_CELL_UV2_PRO_ENABLE        0U

/* 主保护开关关闭时，单体深度欠压保护强制关闭。 */
#define BMS_PROTECT_CELL_DEEP_UV_ENABLE        0U

/* 主保护开关关闭时，总压欠压告警强制关闭。 */
#define BMS_PROTECT_PACK_UV_WARN_ENABLE        0U

/* 主保护开关关闭时，总压欠压保护强制关闭。 */
#define BMS_PROTECT_PACK_UV_PRO_ENABLE         0U

/* 主保护开关关闭时，充电过温告警强制关闭。 */
#define BMS_PROTECT_CHG_OT_WARN_ENABLE         0U

/* 主保护开关关闭时，充电过温保护强制关闭。 */
#define BMS_PROTECT_CHG_OT_PRO_ENABLE          0U

/* 主保护开关关闭时，充电低温告警强制关闭。 */
#define BMS_PROTECT_CHG_UT_WARN_ENABLE         0U

/* 主保护开关关闭时，充电低温保护强制关闭。 */
#define BMS_PROTECT_CHG_UT_PRO_ENABLE          0U

/* 主保护开关关闭时，放电过温告警强制关闭。 */
#define BMS_PROTECT_DSG_OT_WARN_ENABLE         0U

/* 主保护开关关闭时，放电过温保护强制关闭。 */
#define BMS_PROTECT_DSG_OT_PRO_ENABLE          0U

/* 主保护开关关闭时，放电低温告警强制关闭。 */
#define BMS_PROTECT_DSG_UT_WARN_ENABLE         0U

/* 主保护开关关闭时，放电低温保护强制关闭。 */
#define BMS_PROTECT_DSG_UT_PRO_ENABLE          0U

/* 主保护开关关闭时，MOS过温告警强制关闭。 */
#define BMS_PROTECT_MOS_OT_WARN_ENABLE         0U

/* 主保护开关关闭时，MOS过温保护强制关闭。 */
#define BMS_PROTECT_MOS_OT_PRO_ENABLE          0U

/* 主保护开关关闭时，充电MOS失效检测强制关闭。 */
#define BMS_PROTECT_CHG_MOS_FAIL_ENABLE        0U

/* 主保护开关关闭时，放电MOS失效检测强制关闭。 */
#define BMS_PROTECT_DSG_MOS_FAIL_ENABLE        0U

/* 主保护开关关闭时，充电过流告警强制关闭。 */
#define BMS_PROTECT_CHG_OC_WARN_ENABLE         0U

/* 主保护开关关闭时，充电过流保护强制关闭。 */
#define BMS_PROTECT_CHG_OC_PRO_ENABLE          0U

/* 主保护开关关闭时，回馈过流保护强制关闭。 */
#define BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE    0U

/* 主保护开关关闭时，回馈过流告警强制关闭。 */
#define BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE   0U

/* 主保护开关关闭时，充电过流锁定强制关闭。 */
#define BMS_PROTECT_CHG_OC_LOCK_ENABLE         0U

/* 主保护开关关闭时，放电过流告警强制关闭。 */
#define BMS_PROTECT_DSG_OC_WARN_ENABLE         0U

/* 主保护开关关闭时，放电过流保护强制关闭。 */
#define BMS_PROTECT_DSG_OC_PRO_ENABLE          0U

/* 主保护开关关闭时，放电过流锁定强制关闭。 */
#define BMS_PROTECT_DSG_OC_LOCK_ENABLE         0U

/* 主保护开关关闭时，软件短路保护强制关闭。 */
#define BMS_PROTECT_SHORT_ENABLE               0U

/* 主保护开关关闭时，负载移除检测功能强制关闭。 */
#define BMS_LOAD_DETECT_ENABLE                 0U

/* 主保护开关关闭时，单体压差告警强制关闭。 */
#define BMS_PROTECT_CELL_DIFF_WARN_ENABLE      0U

/* 主保护开关关闭时，单体压差保护强制关闭。 */
#define BMS_PROTECT_CELL_DIFF_PRO_ENABLE       0U

/* 主保护开关关闭时，单体压差二级保护强制关闭。 */
#define BMS_PROTECT_CELL_DIFF_PRO2_ENABLE      0U
#else
/*
 * AFE通信/数据有效性保护开关。
 *
 * 开启后，APP会检查AFE数据是否完整、AFE通信看门狗是否异常以及NTC寄存器
 * 通信是否正常，并在异常持续达到配置时间后置位AFE故障。
 * 关闭后，不执行这部分软件数据有效性判断。
 */
#define BMS_PROTECT_AFE_FAULT_ENABLE           1U

/*
 * NTC故障软件保护开关。
 *
 * 开启后，APP根据异常NTC通道数量和当前充放电状态判断NTC故障。
 * NTC异常数量的统计和触发/恢复时间由本文件下面的NTC参数控制。
 * 关闭后，NTC故障保护位不由这条软件保护链路置位。
 */
#define BMS_PROTECT_NTC_FAULT_ENABLE           1U

/*
 * 单体过压告警开关。
 *
 * 开启后，最高单体电压达到或超过COV告警阈值并持续达到告警延时，置位单体过压
 * 告警标志；恢复条件满足后清除。该开关只负责告警，不代替一级/二级过压保护。
 */
#define BMS_PROTECT_CELL_OV_WARN_ENABLE        1U

/*
 * 单体一级过压保护开关。
 *
 * 开启后，最高单体电压达到或超过COV一级保护阈值并持续达到保护延时，置位单体
 * 一级过压保护标志，并参与充电禁止等后续保护处理。
 */
#define BMS_PROTECT_CELL_OV_PRO_ENABLE         1U

/*
 * 单体二级过压保护开关。
 *
 * 开启后，最高单体电压达到或超过COV2二级保护阈值并持续达到二级保护延时，置位
 * 单体二级过压保护标志。它与单体一级过压保护使用不同阈值、延时和计时器。
 */
#define BMS_PROTECT_CELL_OV2_PRO_ENABLE        0U

/*
 * 总压过压告警开关。
 *
 * 开启后，电池总压达到或超过“单体过压告警阈值×当前串数”并持续达到告警延时，
 * 置位总压过压告警标志。该开关独立于单体过压告警开关。
 */
#define BMS_PROTECT_PACK_OV_WARN_ENABLE        0U

/*
 * 总压过压保护开关。
 *
 * 开启后，电池总压达到或超过“单体过压保护阈值×当前串数”并持续达到保护延时，
 * 置位总压过压保护标志。该开关独立于单体过压保护开关。
 */
#define BMS_PROTECT_PACK_OV_PRO_ENABLE         0U

/*
 * 单体欠压告警开关。
 *
 * 开启后，最低单体电压达到或低于CUV告警阈值并持续达到告警延时，置位单体欠压
 * 告警标志；恢复条件满足后清除。该开关只负责告警，不代替欠压保护。
 */
#define BMS_PROTECT_CELL_UV_WARN_ENABLE        1U

/*
 * 单体一级欠压保护开关。
 * 开启后按最低有效电芯温度选择CUV1阈值，独立置位和恢复一级欠压故障。
 */
#define BMS_PROTECT_CELL_UV1_PRO_ENABLE        1U

/* 单体二级欠压保护开关：独立判断CUV2阈值、延时及恢复条件。 */
#define BMS_PROTECT_CELL_UV2_PRO_ENABLE        1U

/*
 * 单体深度欠压保护开关。
 *
 * 开启后，最低单体电压达到或低于深度欠压阈值并持续达到配置延时，置位深度欠压
 * 故障。该故障属于严重欠压路径，不与普通一级/二级欠压保护共用开关。
 */
#define BMS_PROTECT_CELL_DEEP_UV_ENABLE        1U

/*
 * 总压欠压告警开关。
 *
 * 开启后，电池总压达到或低于“单体欠压告警阈值×当前串数”并持续达到告警延时，
 * 置位总压欠压告警标志。该开关独立于单体欠压告警开关。
 */
#define BMS_PROTECT_PACK_UV_WARN_ENABLE        0U

/*
 * 总压欠压保护开关。
 *
 * 开启后，电池总压达到或低于“单体二级欠压保护阈值×当前串数”并持续达到保护延时，
 * 置位总压欠压保护标志。该开关独立于单体欠压保护开关。
 */
#define BMS_PROTECT_PACK_UV_PRO_ENABLE         0U

/* 充电过温告警开关：只在DETC在位时判断，不直接执行保护动作。 */
#define BMS_PROTECT_CHG_OT_WARN_ENABLE         1U

/* 充电过温保护开关：只在DETC在位时判断，并参与充电禁止处理。 */
#define BMS_PROTECT_CHG_OT_PRO_ENABLE          1U

/* 充电低温告警开关：只在充电状态下判断充电低温告警，不直接执行保护动作。 */
#define BMS_PROTECT_CHG_UT_WARN_ENABLE         1U

/* 充电低温保护开关：只在充电状态下判断充电低温保护，并参与充电禁止处理。 */
#define BMS_PROTECT_CHG_UT_PRO_ENABLE          1U

/* 放电过温告警开关：只在非充电状态下判断放电过温告警，不直接执行保护动作。 */
#define BMS_PROTECT_DSG_OT_WARN_ENABLE         1U

/* 放电过温保护开关：只在非充电状态下判断放电过温保护，并参与放电禁止处理。 */
#define BMS_PROTECT_DSG_OT_PRO_ENABLE          1U

/* 放电低温告警开关：只在非充电状态下判断放电低温告警，不直接执行保护动作。 */
#define BMS_PROTECT_DSG_UT_WARN_ENABLE         1U

/* 放电低温保护开关：只在非充电状态下判断放电低温保护，并参与放电禁止处理。 */
#define BMS_PROTECT_DSG_UT_PRO_ENABLE          1U

/* MOS过温告警开关：开启后判断MOS_NTC过温告警，告警状态用于上报和日志记录。 */
#define BMS_PROTECT_MOS_OT_WARN_ENABLE         1U

/* MOS过温保护开关：开启后判断MOS_NTC过温保护，并参与通用故障处理。 */
#define BMS_PROTECT_MOS_OT_PRO_ENABLE          1U

/* 充电MOS失效保护开关：AFE反馈为断开但仍有充电电流持续超限时锁存MOS失效PF。 */
#define BMS_PROTECT_CHG_MOS_FAIL_ENABLE        1U

/* 放电MOS失效保护开关：AFE反馈为断开但仍有放电电流持续超限时锁存MOS失效PF。 */
#define BMS_PROTECT_DSG_MOS_FAIL_ENABLE        1U

/* 充电过流告警开关：开启后按充电电流告警阈值和延时置位充电过流告警。 */
#define BMS_PROTECT_CHG_OC_WARN_ENABLE         1U

/* 充电过流保护开关：开启后按充电电流保护阈值和延时置位一级/二级保护。 */
#define BMS_PROTECT_CHG_OC_PRO_ENABLE          1U

/* 回馈过流保护开关：DETD在位、DETC不在位且充电电流超限时执行保护。 */
#define BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE    1U

/* 回馈过流告警开关：DETD在位、DETC不在位且充电电流超限时置位告警。 */
#define BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE   1U

/*
 * 充电过流锁定开关。
 *
 * 开启后，充电过流保护事件累计达到OCC_LOCK_COUNT_LIMIT时锁定充电过流状态。
 * 锁定逻辑依赖充电过流保护开关；如果充电过流保护关闭，则本锁定开关不会单独
 * 产生保护事件。
 */
#define BMS_PROTECT_CHG_OC_LOCK_ENABLE         1U

/* 放电过流告警开关：开启后按放电电流告警阈值和延时置位放电过流告警。 */
#define BMS_PROTECT_DSG_OC_WARN_ENABLE         1U

/* 放电过流保护开关：开启后按OCD1~OCD4分别判断多个放电过流保护等级。 */
#define BMS_PROTECT_DSG_OC_PRO_ENABLE          1U

/*
 * 放电过流锁定开关。
 *
 * 开启后，放电过流保护或短路保护事件累计达到OCD_LOCK_COUNT_LIMIT时锁定放电；
 * 锁定解除条件由放电保护流程决定。该开关不代替放电过流保护本身。
 */
#define BMS_PROTECT_DSG_OC_LOCK_ENABLE         1U

/* 软件短路保护开关：开启后按PDS短路电流阈值和延时判断软件短路保护。 */
#define BMS_PROTECT_SHORT_ENABLE               0U

/*
 * 负载移除检测开关。
 *
 * 开启后，在放电过流/短路保护发生且检测条件满足时，打开负载检测电路并读取
 * MCU_DET_LOAD；确认外部负载移除后，可以提前清除对应放电保护状态。
 */
#define BMS_LOAD_DETECT_ENABLE                 1U

/* 单体压差告警开关：开启后按最大/最小单体电压差判断压差告警。 */
#define BMS_PROTECT_CELL_DIFF_WARN_ENABLE      1U

/* 单体压差保护开关：开启后按最大/最小单体电压差判断压差保护。 */
#define BMS_PROTECT_CELL_DIFF_PRO_ENABLE       1U

/* 单体压差二级保护开关：在一级保护基础上增加更严重压差的独立判断。 */
#define BMS_PROTECT_CELL_DIFF_PRO2_ENABLE      1U
#endif

/*
 * AFE硬件故障清除配置。
 * 这条清除链路不受软件保护开关控制，因此作为独立公共配置始终保留。
 */
#define AFE_CLEAR_COV_RECOVER_VOLTAGE           3950U
#define AFE_CLEAR_COV_RECOVER_TIME              TMR0_DELAY_SEC(3U)
#define AFE_CLEAR_CUV_RECOVER_VOLTAGE           2400U
#define AFE_CLEAR_CUV_RECOVER_TIME              TMR0_DELAY_SEC(2U)
#define AFE_CLEAR_OCC_RECOVER_CUR_10MA          1
#define AFE_CLEAR_OCC_RECOVER_TIME              TMR0_DELAY_SEC(30U)
#define AFE_CLEAR_OCD_RECOVER_CUR_10MA          (-1000)
#define AFE_CLEAR_OCD_RECOVER_TIME              TMR0_DELAY_SEC(30U)
#define AFE_CLEAR_SCD_RECOVER_TIME              TMR0_DELAY_SEC(30U)
#define AFE_CLEAR_IIC_RECOVER_TIME              TMR0_DELAY_MS(10U)

/*
 * AFE通信/数据有效性保护。
 *
 * 触发条件：
 * 1. AFE数据未读齐。
 * 2. AFE IIC看门狗或通信状态异常。
 *
 * 恢复条件：
 * AFE数据恢复正常，且IIC看门狗状态正常。
 */
#if (BMS_PROTECT_AFE_FAULT_ENABLE != 0U)
#define AFE_FAULT_SET_TIME                     TMR0_DELAY_SEC(3U)   /* 异常持续3s后置位AFE_fault。 */
#define AFE_FAULT_RECOVER_TIME                 TMR0_DELAY_MS(10U)   /* 正常持续10ms后清除软件AFE_fault。 */
#endif

/*
 * NTC异常保护配置。
 *
 * NTC异常数量为1路时只生成协议告警，不进入软件保护；达到配置数量后，
 * 才允许进入NTC故障保护判断。当前统计通道为AFE_NTC、MCU_NTC1、
 * MCU_NTC2、MOS_NTC和PTC_NTC，共5路。
 */
/* 异常数量阈值还供CAN/UART及日志区分NTC告警和故障，属于公共配置。 */
#define BMS_NTC_FAULT_MIN_COUNT                2U      /* 至少2路NTC电气异常才属于NTC-Fault。 */
#if (BMS_PROTECT_NTC_FAULT_ENABLE != 0U)
#define BMS_NTC_PROTECT_IDLE_CURRENT_MA        2000    /* 绝对电流不高于2A且不充放电时才允许保护，单位mA。 */
#define NTC_FAULT_SET_TIME                     TMR0_DELAY_SEC(30U) /* 当前项目要求异常持续30s后置位保护。 */
#define NTC_FAULT_RECOVER_TIME                 TMR0_DELAY_SEC(2U)  /* 异常数量恢复后持续2s清除保护。 */
#endif

/*
 * 单体/总压过压告警与保护。
 *
 * COV表示Cell Over Voltage。
 * WAR为告警，不一定关MOS；PRO为保护，会参与MOS控制。
 * PACK_OV由单体阈值乘以当前串数得到总压阈值，避免不同串数项目重复配置。
 */
#if (BMS_PROTECT_CELL_OV_WARN_ENABLE || BMS_PROTECT_PACK_OV_WARN_ENABLE)
#define COV_SET_WAR_VOLTAGE                    4150U   /* 单体电压达到或超过4150mV并满足延时后置位过压告警。 */
#define COV_SET_WAR_TIME                       TMR0_DELAY_SEC(3U)
#define COV_RECOVER_WAR_VOLTAGE                3950U   /* 单体电压不高于3950mV并满足延时后清除过压告警。 */
#define COV_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(3U)
#endif

#if (BMS_PROTECT_CELL_OV_PRO_ENABLE || BMS_PROTECT_PACK_OV_PRO_ENABLE)
#define COV_SET_PRO_VOLTAGE                    4280U   /* 单体电压达到或超过4280mV并满足延时后置位一级过压保护。 */
#define COV_SET_PRO_TIME                       TMR0_DELAY_SEC(3U)
#define COV_RECOVER_PRO_VOLTAGE                3950U   /* 单体电压不高于3950mV并满足延时后清除一级过压保护。 */
#define COV_RECOVER_PRO_TIME                   TMR0_DELAY_SEC(3U)
#endif

#if (BMS_PROTECT_CELL_OV2_PRO_ENABLE != 0U)
#define COV2_SET_PRO_VOLTAGE                   4350U   /* 单体电压达到或超过4350mV并满足延时后置位二级过压保护。 */
#define COV2_SET_PRO_TIME                      TMR0_DELAY_SEC(3U)
#define COV2_RECOVER_PRO_VOLTAGE               3950U   /* 单体电压不高于3950mV并满足延时后清除二级过压保护。 */
#define COV2_RECOVER_PRO_TIME                  TMR0_DELAY_SEC(3U)
#endif


/*
 * 单体/总压欠压告警与保护。
 *
 * CUV表示Cell Under Voltage。
 * 单体一级欠压保护根据最低有效电芯温度选择三档阈值；运行中温度分界采用
 * 配置回差。温度无效时使用暖档阈值，避免因温度数据丢失而放宽欠压保护。
 * 二级欠压保护保持独立，用于更低电压时快速保护。
 */
#if (BMS_PROTECT_CELL_UV_WARN_ENABLE || BMS_PROTECT_PACK_UV_WARN_ENABLE)
#define CUV_SET_WAR_VOLTAGE                    2900U   /* 单体达到或低于2900mV并满足延时后置位欠压告警。 */
#define CUV_SET_WAR_TIME                       TMR0_DELAY_SEC(2U)
#define CUV_RECOVER_WAR_VOLTAGE                3000U   /* 单体不低于3000mV并满足延时后清除欠压告警。 */
#define CUV_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_CELL_UV1_PRO_ENABLE != 0U)
#define CUV1_TEMP_WARM_BOUNDARY_C              10      /* 暖档/中档分界温度，℃。 */
#define CUV1_TEMP_COLD_BOUNDARY_C              0       /* 中档/冷档分界温度，℃。 */
#define CUV1_TEMP_BAND_HYSTERESIS_C            1       /* 温度档位回差，℃。 */
#define CUV1_SET_PRO_WARM_VOLTAGE_MV           2700U   /* T>=10℃暖档一级欠压置位电压。 */
#define CUV1_SET_PRO_MID_VOLTAGE_MV            2500U   /* 0℃<=T<10℃中档一级欠压置位电压。 */
#define CUV1_SET_PRO_COLD_VOLTAGE_MV           2200U   /* T<0℃冷档一级欠压置位电压。 */
#define CUV1_RECOVER_PRO_WARM_VOLTAGE_MV       3100U   /* 暖档一级欠压解除电压。 */
#define CUV1_RECOVER_PRO_MID_VOLTAGE_MV        2900U   /* 中档一级欠压解除电压。 */
#define CUV1_RECOVER_PRO_COLD_VOLTAGE_MV       2600U   /* 冷档一级欠压解除电压。 */
#define CUV1_SET_PRO_TIME                      TMR0_DELAY_SEC(3U)
#define CUV1_RECOVER_PRO_TIME                  TMR0_DELAY_SEC(2U)
#endif
#if (BMS_PROTECT_CELL_UV2_PRO_ENABLE || BMS_PROTECT_PACK_UV_PRO_ENABLE)
#define CUV2_SET_PRO_VOLTAGE                   2000U   /* 二级欠压保护置位阈值。 */
#define CUV2_SET_PRO_TIME                      TMR0_DELAY_SEC(60U)
#define CUV2_RECOVER_PRO_VOLTAGE               2200U   /* 二级欠压保护恢复阈值。 */
#define CUV2_RECOVER_PRO_TIME                  TMR0_DELAY_SEC(2U)
#endif

#if (BMS_PROTECT_CELL_DEEP_UV_ENABLE != 0U)
#define DCUV_SET_PRO_VOLTAGE                   1500U   /* 单体达到或低于1500mV并满足延时后置位深度欠压保护。 */
#define DCUV_SET_PRO_TIME                      TMR0_DELAY_SEC(30U)
#endif


/*
 * 充电温度保护。
 *
 * OTC表示充电过温，UTC表示充电低温。
 * 当前温度来源是AFE换算后的NTC温度，单位为摄氏度。
 */
#if (BMS_PROTECT_CHG_OT_WARN_ENABLE != 0U)
#define OTC_SET_WAR_TEMP                       53      /* 充电温度达到或超过告警阈值并满足延时后置位。 */
#define OTC_SET_WAR_TIME                       TMR0_DELAY_SEC(5U)
#define OTC_RECOVER_WAR_TEMP                   45      /* 充电温度不高于恢复阈值并满足延时后清除。 */
#define OTC_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_CHG_OT_PRO_ENABLE != 0U)
#define OTC_SET_PRO_TEMP                       55      /* 充电温度达到或超过保护阈值并满足延时后置位。 */
#define OTC_SET_PRO_TIME                       TMR0_DELAY_SEC(5U)
#define OTC_RECOVER_PRO_TEMP                   45      /* 充电温度不高于恢复阈值并满足延时后清除。 */
#define OTC_RECOVER_PRO_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_CHG_UT_WARN_ENABLE != 0U)
#define UTC_SET_WAR_TEMP                       (-5)    /* 充电温度达到或低于低温告警阈值并满足延时后置位。 */
#define UTC_SET_WAR_TIME                       TMR0_DELAY_SEC(5U)
#define UTC_RECOVER_WAR_TEMP                   (0)       /* 充电温度不低于恢复阈值并满足延时后清除。 */
#define UTC_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_CHG_UT_PRO_ENABLE != 0U)
#define UTC_SET_PRO_TEMP                       (-10)    /* 充电温度达到或低于低温保护阈值并满足延时后置位。 */
#define UTC_SET_PRO_TIME                       TMR0_DELAY_SEC(5U)
#define UTC_RECOVER_PRO_TEMP                   (0)       /* 充电温度不低于恢复阈值并满足延时后清除。 */
#define UTC_RECOVER_PRO_TIME                   TMR0_DELAY_SEC(5U)
#endif

/*
 * 放电温度保护。
 *
 * OTD表示放电过温，UTD表示放电低温。
 */
#if (BMS_PROTECT_DSG_OT_WARN_ENABLE != 0U)
#define OTD_SET_WAR_TEMP                       57      /* 放电温度达到或超过过温告警阈值并满足延时后置位。 */
#define OTD_SET_WAR_TIME                       TMR0_DELAY_SEC(5U)
#define OTD_RECOVER_WAR_TEMP                   50      /* 放电温度不高于恢复阈值并满足延时后清除。 */
#define OTD_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_DSG_OT_PRO_ENABLE != 0U)
#define OTD_SET_PRO_TEMP                       60      /* 放电温度达到或超过过温保护阈值并满足延时后置位。 */
#define OTD_SET_PRO_TIME                       TMR0_DELAY_SEC(5U)
#define OTD_RECOVER_PRO_TEMP                   50      /* 放电温度不高于恢复阈值并满足延时后清除。 */
#define OTD_RECOVER_PRO_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_DSG_UT_WARN_ENABLE != 0U)
#define UTD_SET_WAR_TEMP                       (-20)   /* 放电温度达到或低于低温告警阈值并满足延时后置位。 */
#define UTD_SET_WAR_TIME                       TMR0_DELAY_SEC(5U)
#define UTD_RECOVER_WAR_TEMP                   (-15)   /* 放电温度不低于恢复阈值并满足延时后清除。 */
#define UTD_RECOVER_WAR_TIME                   TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_DSG_UT_PRO_ENABLE != 0U)
#define UTD_SET_PRO_TEMP                       (-25)   /* 放电温度达到或低于低温保护阈值并满足延时后置位。 */
#define UTD_SET_PRO_TIME                       TMR0_DELAY_SEC(5U)
#define UTD_RECOVER_PRO_TEMP                   (-15)   /* 放电温度不低于恢复阈值并满足延时后清除。 */
#define UTD_RECOVER_PRO_TIME                   TMR0_DELAY_SEC(5U)
#endif

/*
 * MOS温度告警和保护配置。
 * MOS_NTC温度单位为0.1℃，因此80.0℃、95.0℃和70.0℃分别写成800、950和700。
 * 告警只用于状态上报和日志记录，不参与MOS控制；保护会进入已有的通用故障处理链路。
 * 温度无效时不置位，也不清除已有状态，只有有效温度不高于恢复阈值时才允许清除。
 */
#if (BMS_PROTECT_MOS_OT_WARN_ENABLE != 0U)
#define MOS_OT_SET_WAR_TEMP_0P1C              900
#define MOS_OT_SET_WAR_TIME                   TMR0_DELAY_SEC(5U)
#define MOS_OT_RECOVER_TEMP_0P1C              750
#define MOS_OT_RECOVER_WAR_TIME               TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_MOS_OT_PRO_ENABLE != 0U)
#define MOS_OT_SET_PRO_TEMP_0P1C              980
#define MOS_OT_SET_PRO_TIME                   TMR0_DELAY_SEC(5U)
#define MOS_OT_RECOVER_PRO_TEMP_0P1C          750
#define MOS_OT_RECOVER_PRO_TIME               TMR0_DELAY_SEC(5U)
#endif

/* MOS失效保护：反馈断开且对应方向电流达到设定阈值并持续配置时间后锁存PF故障。 */
#if (BMS_PROTECT_CHG_MOS_FAIL_ENABLE != 0U)
#define CHG_MOS_FAIL_SET_CUR_MA               1000    /* 充电方向电流达到或超过1.00A。 */
#define CHG_MOS_FAIL_SET_TIME                 TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_DSG_MOS_FAIL_ENABLE != 0U)
#define DSG_MOS_FAIL_SET_CUR_MA               (-1000) /* 放电方向电流达到或低于-1.00A。 */
#define DSG_MOS_FAIL_SET_TIME                 TMR0_DELAY_SEC(5U)
#endif

/*
 * 充电过流保护。
 *
 * OCC表示Charge Over Current。
 * 电流单位为10mA，例如2800表示28A。
 * 充电方向为正数。
 */
#if (BMS_PROTECT_CHG_OC_WARN_ENABLE != 0U)
#define OCC_WAR_SET_CUR_10MA                   2300    /* 充电电流达到或超过配置阈值并满足延时后置位过流告警。 */
#define OCC_WAR_SET_TIME                       TMR0_DELAY_SEC(5U)
#define OCC_WAR_RECOVER_CUR_10MA               2000    /* 充电电流小于等于26A并满足延时后清除过流告警。 */
#define OCC_WAR_RECOVER_TIME                   TMR0_DELAY_SEC(30U)
#endif

#if (BMS_PROTECT_CHG_OC_PRO_ENABLE != 0U)
#define OCC1_SET_PRO_CUR_10MA                  2500    /* 一级充电过流保护阈值，25A。 */
#define OCC1_SET_PRO_TIME                      TMR0_DELAY_SEC(30U)
#define OCC2_SET_PRO_CUR_10MA                  3000    /* 二级充电过流保护阈值，30A。 */
#define OCC2_SET_PRO_TIME                      TMR0_DELAY_MS(500U)
#define OCC1_RECOVER_CUR_10MA                  1       /* 充电过流保护恢复电流，接近0A时允许恢复。 */
#define OCC1_RECOVER_TIME                      TMR0_DELAY_SEC(30U)
#endif

#if (BMS_PROTECT_CHG_OC_LOCK_ENABLE && BMS_PROTECT_CHG_OC_PRO_ENABLE)
#define OCC_LOCK_COUNT_LIMIT                   3U      /* 充电过流保护累计达到3次后进入锁定。 */
#endif



/*
 * 回馈过流保护。
 *
 * OCC表示Charge Over Current。
 * 电流单位为10mA，例如2800表示28A。
 * 充电方向为正数。
 */
 #if (BMS_PROTECT_RECHARGE_OCC_WARN_ENABLE != 0U)
#define RECHARGE_OCC_SET_WAR_CUR_10MA                  3500    /* 回馈电流达到或超过配置阈值后置位告警。 */
#define RECHARGE_OCC_SET_WAR_TIME                      TMR0_DELAY_MS(1500U)
#define RECHARGE_OCC_RECOVER_WAR_CUR_10MA              3500
#define RECHARGE_OCC_RECOVER_WAR_TIME                  TMR0_DELAY_MS(1U)
#endif


#if (BMS_PROTECT_RECHARGE_OCC_PRO_ENABLE != 0U)
#define RECHARGE_OCC_SET_PRO_CUR_10MA                  4500    /* 回馈电流达到或超过配置阈值后触发保护。 */
#define RECHARGE_OCC_SET_PRO_TIME                      TMR0_DELAY_SEC(3U)
#define RECHARGE_OCC_RECOVER_PRO_CUR_10MA              4500
#define RECHARGE_OCC_RECOVER_PRO_TIME                  TMR0_DELAY_SEC(3U)
#endif
	


/*
 * 放电过流和短路保护。
 *
 * OCD表示Discharge Over Current。
 * 放电方向为负数，例如-8000表示-80A。
 * PDS当前用于短路/预放短路类快速保护判断。
 */
#if (BMS_PROTECT_DSG_OC_WARN_ENABLE != 0U)
#define OCD1_WAR_SET_CUR_10MA                  (-8000) /* 放电电流达到或低于配置阈值并满足延时后置位一级过流告警。 */
#define OCD1_WAR_SET_TIME                      TMR0_DELAY_SEC(2U)
#define OCD1_WAR_RECOVER_CUR_10MA              (-7000) /* 放电电流不低于恢复阈值并满足延时后清除一级过流告警。 */
#define OCD1_WAR_RECOVER_TIME                  TMR0_DELAY_SEC(5U)

#define OCD2_WAR_SET_CUR_10MA                  (-11000) /* 放电电流达到或低于配置阈值并满足延时后置位二级过流告警。 */
#define OCD2_WAR_SET_TIME                      TMR0_DELAY_SEC(30U)
#define OCD2_WAR_RECOVER_CUR_10MA              (-7000) /* 放电电流不低于恢复阈值并满足延时后清除二级过流告警。 */
#define OCD2_WAR_RECOVER_TIME                  TMR0_DELAY_SEC(5U)
#endif

#if (BMS_PROTECT_DSG_OC_PRO_ENABLE != 0U)
#define OCD1_SET_PRO_CUR_10MA                  (-9500)  /* 一级放电过流阈值，达到或低于该负向电流后触发（单位10mA）。 */
#define OCD1_SET_PRO_TIME                      TMR0_DELAY_SEC(30U) /* 持续15秒后触发一级保护。 */
#define OCD2_SET_PRO_CUR_10MA                  (-13000) /* 二级放电过流阈值，达到或低于该负向电流后触发（单位10mA）。 */
#define OCD2_SET_PRO_TIME                      TMR0_DELAY_SEC(10U) /* 持续6秒后触发二级保护。 */
#define OCD3_SET_PRO_CUR_10MA                  (-18000) /* 三级放电过流阈值，达到或低于该负向电流后触发（单位10mA）。 */
#define OCD3_SET_PRO_TIME                      TMR0_DELAY_MS(300U) /* 持续1秒后触发三级保护。 */
#define OCD4_SET_PRO_CUR_10MA                  (-30000) /* 四级放电过流阈值，达到或低于该负向电流后触发（单位10mA）。 */
#define OCD4_SET_PRO_TIME                      TMR0_DELAY_MS(40U) /* 持续40毫秒后触发四级保护。 */
#endif

#if (BMS_PROTECT_DSG_OC_PRO_ENABLE || BMS_PROTECT_SHORT_ENABLE)
#define OCD_RECOVER_CUR_10MA                   (-1000) /* 放电过流恢复阈值，电流不低于该值后开始计时。 */
#define OCD_RECOVER_TIME                       TMR0_DELAY_SEC(30U)
#endif

#if (BMS_PROTECT_DSG_OC_LOCK_ENABLE != 0U)
#define OCD_LOCK_COUNT_LIMIT                   3U      /* 放电过流与短路保护累计达到3次后进入锁定。 */
#endif
#if (BMS_PROTECT_DSG_OC_LOCK_ENABLE != 0U)
#define OCD_LOCK_IDLE_RECOVER_TIME              TMR0_DELAY_MIN(30U) /* 锁定后无放电电流持续30分钟，清共用锁和累计次数。 */
#endif

#if (BMS_PROTECT_SHORT_ENABLE != 0U)
#define PDS_SET_PRO_CUR_10MA                   -30000   /* 短路电流达到或低于负向阈值后开始计时。 */
#define PDS_SET_PRO_TIME                       TMR0_DELAY_MS(10U)
#define PDS_RECOVER_TIME                       TMR0_DELAY_SEC(30U)
#endif

/*
 * 负载移除快速恢复。
 *
 * 目的：
 * 放电过流或短路保护后，如果外部负载已经移除，不必继续等固定恢复时间，
 * 可以立即清除放电过流类保护和短路保护，提升用户体验。
 *
 * IO说明：
 * MCU_LD_EN用于打开负载检测电路。
 * MCU_DET_LOAD用于读取负载是否移除。
 */
#define BMS_LOAD_DET_EN_ACTIVE_LEVEL           1U      /* MCU_LD_EN有效电平，当前原理图按高电平打开检测电路。 */
#if (BMS_LOAD_DETECT_ENABLE != 0U)
#define BMS_LOAD_REMOVED_LEVEL                 1U      /* MCU_DET_LOAD等于该电平时认为负载已移除。 */
#endif

/*
 * 单体压差保护。
 *
 * 为避免低电量末端正常压差被误判，只有最低单体不低于
 * CELL_DIFF_ENABLE_MIN_CELL_MV 时才启用压差判断。
 */
#if (BMS_PROTECT_CELL_DIFF_WARN_ENABLE || BMS_PROTECT_CELL_DIFF_PRO_ENABLE || BMS_PROTECT_CELL_DIFF_PRO2_ENABLE)
#define CELL_DIFF_ENABLE_MIN_CELL_MV           3000U   /* 最低单体不低于3000mV时才允许压差告警/保护判断。 */
#endif
#if (BMS_PROTECT_CELL_DIFF_WARN_ENABLE != 0U)
#define CELL_DIFF_WAR_SET_MV                   500U    /* 最大最小单体压差达到或超过配置阈值并满足延时后置位压差告警。 */
#define CELL_DIFF_WAR_SET_TIME                 TMR0_DELAY_SEC(10U)
#define CELL_DIFF_WAR_REC_MV                   300U    /* 压差恢复到300mV以下并满足延时后清除压差告警。 */
#define CELL_DIFF_WAR_REC_TIME                 TMR0_DELAY_SEC(10U)
#endif

#if (BMS_PROTECT_CELL_DIFF_PRO_ENABLE != 0U)
#define CELL_DIFF_PRO_SET_MV                   700U   /* 一级保护：压差达到或超过配置阈值并满足延时后置位。 */
#define CELL_DIFF_PRO_SET_TIME                 TMR0_DELAY_SEC(10U)
#define CELL_DIFF_PRO_REC_MV                   300U    /* 一级保护：压差恢复到150mV以下并满足延时后清除。 */
#define CELL_DIFF_PRO_REC_TIME                 TMR0_DELAY_SEC(10U)
#endif

#if (BMS_PROTECT_CELL_DIFF_PRO2_ENABLE != 0U)
#define CELL_DIFF_PRO2_SET_MV                  1000U   /* 二级保护：压差达到或超过配置阈值并满足延时后置位。 */
#define CELL_DIFF_PRO2_SET_TIME                TMR0_DELAY_SEC(10U)
#endif

#endif
