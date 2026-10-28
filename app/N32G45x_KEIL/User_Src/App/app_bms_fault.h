#ifndef __APP_BMS_FAULT_H_
#define __APP_BMS_FAULT_H_

#include <stdint.h>

/* 充电故障A组：单体电压、充电温度、充电电流和满充标志。 */
typedef struct
{
    uint16_t b1cell_war_cov     : 1; /* 单体过压告警。 */
    uint16_t b1cell_pro_cov     : 1; /* 单体过压保护。 */
    uint16_t b1pack_war_otc     : 1; /* 充电高温告警。 */
    uint16_t b1pack_pro_otc     : 1; /* 充电高温保护。 */
    uint16_t b1pack_war_utc     : 1; /* 充电低温告警。 */
    uint16_t b1pack_pro_utc     : 1; /* 充电低温保护。 */
    uint16_t b1pack_war_occ     : 1; /* 充电过流告警。 */
    uint16_t b1pack_pro_occ1    : 1; /* 一级充电过流保护。 */
    uint16_t b1cell_pro_cov2    : 1; /* 二级单体过压保护。 */
    uint16_t b1pack_pro_orcc    : 1; /* 反向充电电流保护。 */
    uint16_t b1full_charge_flag : 1; /* 满充锁存标志。 */
    uint16_t b1pack_pro_occ2    : 1; /* 二级充电过流保护，复用原有预留位。 */
    uint16_t b1pack_war_orcc    : 1; /* 回馈过流告警。 */
    uint16_t b3unused           : 3;
} t_charge_fault_A;

typedef union
{
    uint16_t u16fault_A;             /* 16位原始访问，用于存储或通信。 */
    t_charge_fault_A tcharge_fault_A;/* 位域访问，用于应用逻辑。 */
} u_charge_fault_A;

/* 充电故障B组：总压和电流锁定类故障，与A组分开保存。 */
typedef struct
{
    uint16_t b1pack_war_pov  : 1; /* 总压过压告警。 */
    uint16_t b1pack_pro_pov  : 1; /* 总压过压保护。 */
    uint16_t b1pack_occ_lock : 1; /* 充电过流锁定。 */
    uint16_t b1reserved_roc_war : 1; /* 未使用，保留原位号以维持后续字段布局。 */
    uint16_t b1reserved_roc_pro : 1; /* 当前无置位来源，保留原位号以维持后续字段布局。 */
    uint16_t b3unnused1      : 3;
    uint16_t b8unnused2      : 8;
} t_charge_fault_B;

typedef union
{
    uint16_t u16fault_B;             /* 16位原始访问，用于存储或通信。 */
    t_charge_fault_B tcharge_fault_B;/* 位域访问，用于应用逻辑。 */
} u_charge_fault_B;

/* 放电故障A组：单体欠压、放电温度和统一放电过流告警。 */
typedef struct
{
    uint16_t b1cell_war_cuv  : 1; /* 单体欠压告警。 */
    uint16_t b1cell_pro_cuv1 : 1; /* 单体一级欠压保护。 */
    uint16_t b1cell_deep_cuv : 1; /* 深度欠压锁存。 */
    uint16_t b1pack_war_otd  : 1; /* 放电高温告警。 */
    uint16_t b1pack_pro_otd  : 1; /* 放电高温保护。 */
    uint16_t b1pack_war_utd  : 1; /* 放电低温告警。 */
    uint16_t b1pack_pro_utd  : 1; /* 放电低温保护。 */
    uint16_t b1pack_war_ocd  : 1; /* 唯一放电过流告警。 */
    uint16_t b3unused_ocd_war : 3; /* 原分级告警位预留，不再对外表达。 */
    uint16_t b1pack_ocd_lock : 1; /* 放电过流锁定。 */
    uint16_t b1low_soc       : 1; /* 低SOC告警/保护来源。 */
    uint16_t b1cell_pro_cuv2 : 1; /* 单体二级欠压保护，复用原有预留位。 */
    uint16_t b2unused       : 2;
} t_discharge_fault_A;

typedef union
{
    uint16_t u16fault_A;                     /* 16位原始访问，用于存储或通信。 */
    t_discharge_fault_A tdischarge_fault_A;  /* 位域访问，用于应用逻辑。 */
} u_discharge_fault_A;

/* 放电故障B组：多级放电过流保护、短路和预放电故障。 */
typedef struct
{
    uint16_t b1pack_pro_ocd1      : 1; /* 一级放电过流保护。 */
    uint16_t b1pack_war_cuv       : 1; /* 总压欠压告警。 */
    uint16_t b1pack_pro_cuv       : 1; /* 总压欠压保护。 */
    uint16_t b1pack_pro_ocd2      : 1; /* 二级放电过流保护，复用原有预留位。 */
    uint16_t b1pack_pro_ocd3      : 1; /* 三级放电过流保护，复用原有预留位。 */
    uint16_t b1pack_pro_ocd4      : 1; /* 四级放电过流保护，复用原有预留位。 */
    uint16_t b1pack_shrot         : 1; /* 电池包短路保护。 */
    uint16_t b1predischarge_fault : 1; /* 预放电故障。 */
    uint16_t b1predischarge_lock  : 1; /* 预放电锁定。 */
    uint16_t b7unnused            : 7;
} t_discharge_fault_B;

typedef union
{
    uint16_t u16fault_B;                     /* 16位原始访问，用于存储或通信。 */
    t_discharge_fault_B tdischarge_fault_B;  /* 位域访问，用于应用逻辑。 */
} u_discharge_fault_B;

/* 通用故障A组：AFE、MOS、二级硬件保护和PF来源。 */
typedef struct
{
    uint16_t b1AFE_fault                 : 1; /* AFE通信/数据/硬件故障。 */
    uint16_t b1cell_voltage_diff_waring : 1; /* 单体压差告警。 */
    uint16_t b1cell_voltage_diff_fault  : 1; /* 单体压差一级保护。 */
    uint16_t b1CHG_MOS_failure          : 1; /* 充电MOS失效。 */
    uint16_t b1DSG_MOS_faulure          : 1; /* 放电MOS失效。 */
    uint16_t b1MOS_OTP                  : 1; /* MOS过温保护。 */
    uint16_t b1hardware_second_pro      : 1; /* 二级硬件保护输入。 */
    uint16_t b1SCP_FUSE_fusing          : 1; /* 短路熔断标志。 */
    uint16_t b1MOS_OTP_waring           : 1; /* MOS过温告警。 */
    uint16_t b1MOS_OTP_FUSE             : 1; /* MOS过温熔断条件。 */
    uint16_t b1OTD_FUSE                 : 1; /* 放电过温熔断条件。 */
    uint16_t b1NTC_fault                : 1; /* 至少两路NTC电气异常且电池空闲后置位的NTC保护。 */
    uint16_t b1cell_voltage_diff_fault2 : 1; /* 单体压差二级保护，使用原预留位。 */
    uint16_t b3unnused                  : 3;
} t_general_fault_A;

typedef union
{
    uint16_t u16fault_A;                 /* 16位原始访问，用于存储或通信。 */
    t_general_fault_A tgeneral_fault_A;  /* 位域访问，用于应用逻辑。 */
} u_general_fault_A;

/* 通用故障B组预留，用于协议和存储兼容。 */
typedef struct
{
    uint16_t b16unused;
} t_general_fault_B;

typedef union
{
    uint16_t u16fault_B;                 /* 16位原始访问，用于存储或通信。 */
    t_general_fault_B tgeneral_fault_B;  /* 位域访问，用于应用逻辑。 */
} u_general_fault_B;

/* 完整BMS故障图，按充电、放电、通用故障分组。 */
typedef struct
{
    u_charge_fault_A ucharge_fault_A;
    u_charge_fault_B ucharge_fault_B;
    u_discharge_fault_A udischarge_fault_A;
    u_discharge_fault_B udischarge_fault_B;
    u_general_fault_A ugeneral_fault_A;
    u_general_fault_B ugeneral_fault_B;
} t_bms_fault_flag;

/* 汇总标志，供MOS控制和上层状态机使用。 */
typedef struct
{
    uint8_t u8charge_pro_flag;    /* 任一需要禁止充电的故障。 */
    uint8_t u8discharge_pro_flag; /* 任一需要禁止放电的故障。 */
    uint8_t u8general_fault_flag; /* 任一非方向性的通用故障。 */
    uint8_t u8PF_fault_flag;      /* 永久故障类来源。 */
    uint8_t u8unused;
} t_bms_fault_stat;

/* 运行时故障上下文：软件故障、最终故障、汇总状态和保存镜像。 */
struct _app_bms_fault_t
{
    uint8_t u8data_init_flag;              /* 最近一次故障处理中得到的AFE数据有效标志。 */
    t_bms_fault_flag tbms_sw_fault_flag;   /* 软件阈值计算出的故障。 */
    t_bms_fault_flag tbms_fault_flag;      /* 软件故障与AFE硬件故障合并后的最终故障。 */
    t_bms_fault_stat tbms_fault_stat;      /* MOS控制使用的故障汇总标志。 */
    t_bms_fault_flag tsave_bms_fault_flag; /* 上次保存的故障镜像，用于变化检测。 */
    uint32_t u32overcharge_count;           /* 单体/总压过充保护事件次数，掉电保存在PF页。 */
    uint32_t u32overdischarge_count;        /* 单体/总压过放保护事件次数，掉电保存在PF页。 */
};

extern struct _app_bms_fault_t gt_app_bms_fault;

/* 复制最终故障图和汇总状态，读取方不得直接持有全局对象指针。 */
void app_bms_fault_snapshot_read(struct _app_bms_fault_t *snapshot);

/* AFE单体数据足够用于保护和MOS判断时返回1。 */
uint8_t app_bms_fault_data_ready(void);
/* 上电时读取APP database里保存的PF锁存来源，并恢复到故障模块。 */
void app_bms_fault_load_shutdown_pf(void);
/* shutdown、复位前、升级前保存当前PF锁存来源和过充/过放次数；返回1表示保存成功。 */
uint8_t app_bms_fault_save_shutdown_pf(void);
/* 低优先级后台持久化入口；仅在电压保护事件计数变更后写入PF页。 */
void app_bms_fault_storage_task(void);
/* 清除当前运行态的全部软件故障、最终故障和汇总状态；不改变故障判断配置。 */
void app_bms_fault_clear_all(void);
/* 清除APP database中的PF记录和过充/过放次数；返回1表示Flash擦除成功。 */
uint8_t app_bms_fault_clear_shutdown_pf(void);
/* 只更新软件AFE故障位。 */
void app_bms_fault_update_afe(void);
/* 将软件故障和AFE硬件故障合并到最终故障图。 */
void app_bms_fault_merge(void);
/* 返回当前最终故障对应的PF来源位图。 */
uint32_t app_bms_fault_get_pf_mask(void);
/* 返回当前二级熔断输出命令，1表示输出有效。 */
uint8_t app_bms_fault_get_efuse_cmd(void);
/* 执行一次完整故障处理流程。 */
void app_bms_fault_task(uint8_t u8device_id);
#endif
