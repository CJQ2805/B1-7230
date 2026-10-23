#ifndef __APP_BMS_TYPES_H__
#define __APP_BMS_TYPES_H__

#include <stdbool.h>
#include <stdint.h>
#include "App/app_input_signals.h"
#include "App/app_bms_fault.h"

#include "App/app_param.h"

/* 温度通道总数由硬件NTC配置推导，不作为独立项目参数维护。 */
#define APP_BMS_MAX_TEMP_NUM (AFE_NTC_NUM + MCU_NTC_NUM)
#define MAX_TEMP_NUM APP_BMS_MAX_TEMP_NUM

/* 无效温度数值哨兵；使用温度前仍须检查对应有效位/有效数量。 */
#define APP_BMS_TEMP_INVALID_VALUE ((int16_t)0x7FFF)

/*
 * BMS业务公共类型。
 *
 * 这个头文件只定义业务层共同使用的数据结构，不依赖任何具体芯片、具体引脚或具体AFE型号。
 * 后续故障、MOS控制、LOG、通信和低功耗模块都优先使用这些结构体交互，避免直接读取其他模块内部变量。
 */

/*
 * AFE硬件故障位：这里是业务抽象后的状态，不暴露具体AFE寄存器位名。
 * 当 snapshot->afe_status_valid == 0U 时，这些字段仅表示本轮缓存值，不应被当成有效硬件状态。
 */
typedef struct
{
    uint8_t cell_ov;        /* 单体过压硬件保护。 */
    uint8_t cell_uv;        /* 单体欠压硬件保护。 */
    uint8_t chg_oc1;        /* 充电一级过流硬件保护。 */
    uint8_t chg_oc2;        /* 充电二级过流硬件保护。 */
    uint8_t dsg_oc1;        /* 放电一级过流硬件保护。 */
    uint8_t dsg_oc2;        /* 放电二级过流硬件保护。 */
    uint8_t short_circuit;  /* 短路硬件保护。 */
    uint8_t iic_watchdog;   /* AFE IIC看门狗或通信异常。 */
    uint8_t ntc_iic_fault;  /* AFE_NTC寄存器读取失败，属于AFE通信异常，不计入NTC数量。 */
    uint8_t charge_detect;  /* AFE检测到充电器接入。 */
} app_bms_afe_fault_t;

/* NTC数据总线：业务层只关心结果，不依赖具体ADC通道。 */
#define APP_BMS_NTC_VALID_AFE        ((uint8_t)0x01U)
#define APP_BMS_NTC_VALID_CELL1      ((uint8_t)0x02U)
#define APP_BMS_NTC_VALID_CELL2      ((uint8_t)0x04U)
#define APP_BMS_NTC_VALID_MOS        ((uint8_t)0x08U)
#define APP_BMS_NTC_VALID_PTC        ((uint8_t)0x10U)
#define APP_BMS_NTC_VALID_MCU        ((uint8_t)0x20U)
#define APP_BMS_NTC_VALID_EXTERNAL   ((uint8_t)0x1FU)

#define APP_BMS_NTC_FAULT_AFE        ((uint8_t)0x01U)
#define APP_BMS_NTC_FAULT_CELL1      ((uint8_t)0x02U)
#define APP_BMS_NTC_FAULT_CELL2      ((uint8_t)0x04U)
#define APP_BMS_NTC_FAULT_MOS        ((uint8_t)0x08U)
#define APP_BMS_NTC_FAULT_PTC        ((uint8_t)0x10U)

typedef struct
{
    int16_t afe_temp_0p1c;
    int16_t cell_ntc1_0p1c;
    int16_t cell_ntc2_0p1c;
    int16_t mos_temp_0p1c;
    int16_t ptc_temp_0p1c;
    int16_t mcu_temp_0p1c;

    uint16_t cell_ntc1_raw;
    uint16_t cell_ntc2_raw;
    uint16_t mos_raw;
    uint16_t ptc_raw;
    uint16_t mcu_temp_raw;

    uint32_t cell_ntc1_res_ohm;
    uint32_t cell_ntc2_res_ohm;
    uint32_t mos_res_ohm;
    uint32_t ptc_res_ohm;

    uint8_t valid_mask;
    uint8_t fault_mask;
    uint8_t short_mask;
    uint8_t open_mask;
    uint8_t all_valid;
} app_bms_ntc_state_t;

/* 故障镜像保留原始联合体，便于调试器按故障名称查看具体位；摘要标志供业务快速判断。 */
typedef struct
{
    t_bms_fault_flag detail; /* 充/放电及通用故障联合体；调试器可展开到具体命名位。 */
    uint8_t charge_pro;
    uint8_t discharge_pro;
    uint8_t general_fault;
    uint8_t pf_fault;
} app_bms_fault_image_t;

typedef struct
{
    app_bms_fault_image_t software; /* 纯软件计算故障。 */
    app_bms_fault_image_t active;   /* 软件与硬件合并后的当前故障。 */
    app_bms_fault_image_t saved;    /* 上次保存的故障镜像，用于变化对照。 */
} app_bms_fault_state_t;

typedef struct
{
    double s32total_accumulate_charge_cap;
    double s32total_accumulate_discharge_cap;
    double s32single_accumulate_charge_cap;
    double s32single_accumulate_discharge_cap;
    double dChgDsgAccumCap;
} app_bms_soc_accumulate_capacity_t;

typedef struct
{
    int8_t s8stock_soc;
    int8_t s8real_soc;
} app_bms_soc_compensate_t;

typedef struct
{
    int32_t s32real_cap_val;
    int32_t s32display_cap_val;
    int32_t s32cap_cnt;
} app_bms_soc_capacity_t;

typedef struct
{
    uint8_t u8handle_soc;
    uint8_t u8fcc_condition_flag;
} app_bms_soc_fcc_t;

typedef struct
{
    uint8_t u8SocInit;
    uint16_t u16FullChgCapBuf[5];
    double dCycleCap;
} app_bms_soc_fcc_update_t;

typedef struct
{
    app_bms_soc_accumulate_capacity_t tsoc_accumulate_cap_data;
    app_bms_soc_compensate_t tsoc_compensate_para;
    app_bms_soc_capacity_t tsoc_capacity_data;
    app_bms_soc_fcc_t tsoc_fcc_data;
    app_bms_soc_fcc_update_t tsoc_fcc_update;
    int32_t s32battery_full_cap;
    uint8_t u8display_soc;
    uint8_t u8display_soh;
    uint8_t u8soc_fcc_flag;
    int32_t s32remain_cap;
    uint32_t u32cycle_index;
    uint32_t u32tim_250ms_cnt;
} app_bms_soc_state_t;

/*
 * MOS和外部功率路径状态，统一给通信、LOG、低功耗和后续控制模块读取。
 * chg_status/dsg_status 表示业务策略本轮是否允许该路径导通；
 * chg_mos_state/dsg_mos_state/pre_mos_state 表示反馈值或最近一次输出状态。
 */
typedef struct
{
    uint8_t chg_status;     /* 充电路径业务允许状态，不等同于MOS一定已闭合。 */
    uint8_t dsg_status;     /* 放电路径业务允许状态，不等同于MOS一定已闭合。 */
    uint8_t chg_mos_state;  /* 充电MOS反馈位；当前来自AFE状态寄存器。 */
    uint8_t dsg_mos_state;  /* 放电MOS反馈位；当前来自AFE状态寄存器。 */
    uint8_t pre_mos_state;  /* 预放MOS当前状态；无独立反馈时取功率模块最近一次输出。 */
    uint8_t chg_mos_cmd;
    uint8_t dsg_mos_cmd;
    uint8_t pre_mos_cmd;
    uint8_t efuse_cmd;
    uint8_t pdsg_step;
} app_bms_mos_state_t;

/*
 * SOP限制值的通信快照。
 *
 * CAN只读取这一份副本，不直接读取gt_app_sop；这样协议层不会与SOX任务的
 * 运行对象交叉耦合。电流统一保留现有0.1A量纲，发送时再按协议换算为0.01A。
 */
typedef struct
{
    int16_t s16MaxDischargeCurrent10mA;
    int16_t s16MaxChargeCurrent10mA;
    uint16_t u16MaxRechargeCurrent10mA;
    uint32_t u32MaxChargeVoltageMv;
} app_bms_sop_state_t;

/* 板级外部信号，字段只表达业务含义，不表达具体端口和引脚。 */
typedef struct
{
    app_input_signals_t input;  /* APP全部外部输入的统一快照。 */
} app_bms_signal_t;

/* OBC接收状态：状态值为0=不在位、1=在位、2=故障；原始帧保留最近一次有效报文。 */
#define APP_BMS_OBC_FRAME_SIZE          ((uint8_t)8U)
#define APP_BMS_OBC_STATE_NOT_PRESENT   ((uint8_t)0U)
#define APP_BMS_OBC_STATE_PRESENT       ((uint8_t)1U)
#define APP_BMS_OBC_STATE_FAULT         ((uint8_t)2U)
typedef struct
{
    uint8_t status;
    uint8_t frame_valid;
    uint32_t frame_sequence;                     /* DETC在位期间每收到一帧有效0x393报文递增。 */
    uint8_t raw_data[APP_BMS_OBC_FRAME_SIZE];
} app_bms_obc_state_t;

/* 统一业务快照：当前项目硬件状态被转换成这一份数据后，再提供给各业务模块。 */
typedef struct
{
    /*
     * cell_count / valid_cells / data_ready 的关系：
     *   1. cell_count：本轮应参与业务判断的串数，来自数据源配置并在适配层做上限钳位。
     *   2. valid_cells：在 [0, cell_count) 范围内电压非0的单体数量。
     *   3. data_ready：只有底层数据源声明就绪，且 valid_cells == cell_count 时才置1。
     */
    uint8_t cell_count;                         /* 本轮逻辑业务串数；数组长度与APP_BMS_CELL_COUNT一致。 */
    uint8_t data_ready;                         /* 单体数据完整就绪标志；0表示业务不能把 cell/max/min/gap 当成完整采样。 */
    uint8_t valid_cells;                        /* 本轮已采到非0电压的单体数量，只统计前 cell_count 路。 */
    uint16_t cell_mv[APP_BMS_CELL_COUNT];       /* 逻辑单体电压，单位mV；不包含AFE未接入的扫描空位。 */
    uint16_t max_cell_mv;                       /* 最大单体电压，单位mV。 */
    uint16_t min_cell_mv;                       /* 最小单体电压，单位mV。 */
    uint8_t max_cell_num;                       /* 最大单体序号，0基。 */
    uint8_t min_cell_num;                       /* 最小单体序号，0基。 */
    uint16_t cell_gap_mv;                       /* 最大最小单体压差，单位mV。 */

    uint32_t bat_mv;                            /* 完整有效的逻辑单体电压累加总压，单位mV。 */
    uint32_t vbat_mv;                           /* AFE VBAT采样，单位mV。 */
    int32_t current_ma;                         /* 实际电流，单位mA；保留校准后小电流，不套1A/2A阈值置零；正数充电，负数放电。 */
    uint32_t afe_current_update_seq;            /* AFE电流成功刷新序号；用于唤醒后确认已取得新样本。 */
    int16_t current_10ma;                       /* 业务电流，单位10mA；供现有保护阈值和方向判断使用，允许保留零死区/量化口径。 */
    uint8_t charging;                           /* 充电方向标志；当前由 current_10ma > 0 推导，不额外读取充电器状态位。 */
    uint8_t discharging;                        /* 放电方向标志；当前由 current_10ma < 0 推导。 */
    uint8_t bms_work_status;                    /* 统一工作状态：0静置、1放电、2充电、3回馈。 */

    uint8_t temp_count;                         /* 有效电芯温度数量；当前只统计 AFE_NTC + CELL_NTC1 + CELL_NTC2，不含 MOS/PTC。 */
    int16_t temp_c[APP_BMS_MAX_TEMP_NUM];       /* 电芯温度，单位℃；无效通道为 APP_BMS_TEMP_INVALID_VALUE，0 表示有效的0℃。 */
    int16_t temp_0p1c[APP_BMS_MAX_TEMP_NUM];    /* 电芯温度，单位0.1℃；无效通道为 APP_BMS_TEMP_INVALID_VALUE。 */
    uint8_t temp_valid_mask;                    /* 温度有效位图；判断通道有效性的依据，不能只看数值哨兵。 */
    int16_t max_temp_c;                         /* 最大有效温度，单位℃；无有效温度时为 APP_BMS_TEMP_INVALID_VALUE。 */
    int16_t min_temp_c;                         /* 最小有效温度，单位℃；无有效温度时为 APP_BMS_TEMP_INVALID_VALUE。 */
    uint8_t max_temp_num;                       /* 最大温度序号，0基。 */
    uint8_t min_temp_num;                       /* 最小温度序号，0基。 */

    app_bms_afe_fault_t afe_fault;              /* AFE硬件故障抽象状态。 */
    uint8_t afe_status_valid;                   /* AFE状态寄存器本轮是否读到有效值；0时 afe_fault/chg_mos_state/dsg_mos_state 只可作缓存参考。 */
    uint8_t ntc_abnormal_count;                 /* 由ntc.fault_mask汇总的5路NTC电气异常数量。 */
    app_bms_ntc_state_t ntc;
    app_bms_fault_state_t fault;
    app_bms_soc_state_t soc;
    app_bms_mos_state_t mos;                    /* MOS和功率路径状态。 */
    app_bms_sop_state_t sop;                    /* SOP限制值快照。 */
    app_bms_signal_t signal;                    /* 板级外部信号。 */
    app_bms_obc_state_t obc;                     /* OBC最近一帧及在位/故障状态。 */
    uint32_t reset_reason;                      /* MCU复位原因预留。 */
} app_bms_snapshot_t;

/* 业务模块输出的统一硬件控制命令，由硬件动作适配层负责真正执行。 */
typedef struct
{
    uint8_t chg_mos_enable;       /* 请求充电MOS闭合。 */
    uint8_t dsg_mos_enable;       /* 请求放电MOS闭合。 */
    uint8_t pre_mos_enable;       /* 请求预放MOS闭合。 */
    uint8_t clear_afe_oc_fault;   /* 请求清除AFE过流类故障。 */
    uint8_t clear_afe_sc_fault;   /* 请求清除AFE短路故障。 */
    uint8_t request_sleep;        /* 请求进入低功耗。 */
} app_bms_control_t;

#endif
