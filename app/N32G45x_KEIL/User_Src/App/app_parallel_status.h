#ifndef __APP_PARALLEL_STATUS_H__
#define __APP_PARALLEL_STATUS_H__

#include <stdint.h>

#include "App/app_id_assign.h"
#include "App/app_param.h"
#include "Driver/d_can.h"

/*
 * 对端在线超时门限。last_update_ms使用TIM6毫秒计数；与当前时间做无符号差，
 * 因此计数器自然回绕时仍可比较经过时间。连续5秒没收到任何有效PGN即清槽。
 */
#define APP_PARALLEL_STATUS_OFFLINE_TIMEOUT_MS ((uint32_t)5000UL)

/*
 * B1状态PGN有效位表。PGN范围F20D~F221是连续数值区间，共21帧，
 * valid_pgn_mask的bit号 = PGN - 0xF20D。对端槽位中，该位表示至少收到过该PGN一次；
 * 本机槽位中，该位表示对应字段组已由业务快照填充。它不表示字段非零；合法零值也有效。
 */
#define APP_PARALLEL_STATUS_PGN_BIT_F20D ((uint32_t)1UL << 0U)  /* F20D：总压/电流/SOC。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F20E ((uint32_t)1UL << 1U)  /* F20E：SOP限值和单体极值。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F20F ((uint32_t)1UL << 2U)  /* F20F：单体1~4电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F210 ((uint32_t)1UL << 3U)  /* F210：单体5~8电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F211 ((uint32_t)1UL << 4U)  /* F211：单体9~12电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F212 ((uint32_t)1UL << 5U)  /* F212：单体13~16电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F213 ((uint32_t)1UL << 6U)  /* F213：单体17~20电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F214 ((uint32_t)1UL << 7U)  /* F214：单体21~24电压。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F215 ((uint32_t)1UL << 8U)  /* F215：四路电芯温度。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F216 ((uint32_t)1UL << 9U)  /* F216：温度极值和数量。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F217 ((uint32_t)1UL << 10U) /* F217：循环和容量统计。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F218 ((uint32_t)1UL << 11U) /* F218：插接、MOS、KEY和工作状态。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F219 ((uint32_t)1UL << 12U) /* F219：充放电限值。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21A ((uint32_t)1UL << 13U) /* F21A：告警位图帧1。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21B ((uint32_t)1UL << 14U) /* F21B：告警位图帧2。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21C ((uint32_t)1UL << 15U) /* F21C：告警位图帧3。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21D ((uint32_t)1UL << 16U) /* F21D：保护位图帧1。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21E ((uint32_t)1UL << 17U) /* F21E：保护位图帧2。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F21F ((uint32_t)1UL << 18U) /* F21F：保护位图帧3。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F220 ((uint32_t)1UL << 19U) /* F220：欠压、事件和时间计数。 */
#define APP_PARALLEL_STATUS_PGN_BIT_F221 ((uint32_t)1UL << 20U) /* F221：均衡位图。 */

/*
 * 单个BMS地址槽位的通信状态快照，供后续并联策略读取。
 * 本机与对端使用同一结构：本机由业务快照刷新；对端由F20D~F221接收帧更新。
 * 对端online表示最近5秒内至少接收过该源地址的一帧有效PGN；本机online由快照刷新置位。
 * last_update_ms对端记录最近收帧时间，本机记录最近刷新时间，均使用TIM6毫秒计数。
 * source_address保存CAN源地址；槽位索引 = source_address - DUCAR_CAN_BMS_A_ADDRESS。
 * 字段按注释注明单位；告警/保护帧不在此处拆位，原样保存8字节，避免引入第二套故障定义。
 * 此结构只是数据缓存，不意味着本模块已经执行了并联限流、并联故障仲裁或MOS控制。
 */
typedef struct
{
    uint8_t online;              /* 槽位在线标志：对端有有效帧且未超过5秒；本机快照刷新时置1。 */
    uint8_t source_address;      /* 对应的BMS源地址，地址表范围为基地址起连续15个地址。 */
    uint32_t last_update_ms;     /* 最近一帧有效PGN到达时间，单位ms，来自d_timer_get_ms()。 */
    uint32_t valid_pgn_mask;     /* 按上方位定义记录已收到的PGN，区分未收到与有效零值。 */
    uint32_t pgn_last_update_ms[21]; /* 每个PGN最近一次有效更新时间；只在对应valid位已置位后使用。 */

    /* F20D：Data0~1总压(0.01V)，Data2~3电流(0.01A，有符号)，Data4~5 SOC(0.1%)。 */
    uint16_t total_voltage_001v; /* 电池包总压；例：7200表示72.00V。 */
    int16_t current_001a;        /* 业务电流；正负方向按B1协议符号约定，单位0.01A。 */
    uint16_t soc_0p1_percent;    /* 0.1%分辨率，例如1000代表100.0%。 */
    uint8_t soh_percent;         /* SOH整数百分数。 */
    uint8_t detc_status;         /* DETC状态原协议值；本机按发送协议写1/2。 */

    /* F20E：最大允许电流和单体极值；F20F~F214逐帧携带4节电压，均按mV缓存。 */
    uint16_t max_discharge_current_001a; /* 最大放电电流，单位0.01A。 */
    uint16_t max_charge_current_001a;    /* 最大充电电流，单位0.01A。 */
    uint16_t max_cell_mv;                /* 当前最高单体电压，单位mV。 */
    uint16_t min_cell_mv;                /* 当前最低单体电压，单位mV。 */
    uint16_t cell_mv[APP_BMS_MAX_CELL_NUM]; /* 逻辑单体电压；未收到或未配置的槽位为无效标记。 */

    /* F215/F216：电芯温度和极值使用有符号0.1℃；MOS/PTC使用有符号整数℃。 */
    int16_t cell_temp_0p1c[4]; /* F215四路NTC值；本机只填有效且未报故障的通道。 */
    int8_t mos_temp_c;         /* MOS温度，-128为无效标记。 */
    int8_t ptc_temp_c;         /* PTC温度，-128为无效标记。 */
    int16_t max_cell_temp_0p1c; /* 有效电芯NTC极大值；无有效传感器时为缓存无效标记。 */
    int16_t min_cell_temp_0p1c; /* 有效电芯NTC极小值；无有效传感器时为缓存无效标记。 */
    uint8_t cell_count;        /* 当前包逻辑电芯串数。 */
    uint8_t ntc_count;         /* 当前包配置的电芯NTC数量，不包含MOS/PTC。 */

    /* F217：循环/半充计数单位为次；满充容量和剩余容量以0.01Ah为单位。 */
    uint16_t cycle_count;             /* 累计循环次数。 */
    uint16_t fcc_001ah;               /* Full Charge Capacity，单位0.01Ah。 */
    uint16_t remaining_capacity_001ah; /* 剩余容量，单位0.01Ah。 */
    uint16_t half_charge_count;       /* 半充次数；本机当前没有业务计数源，按既有默认值缓存。 */

    /*
     * F218 Data0 bit0..7依次为DETD、DETC、BMR（并联对端检测）、ID、放电MOS、充电MOS、预放MOS、KEY；
     * 后面的字节分别保存协议并联MOS状态、充/放电告警等级和工作状态。
     */
    uint8_t detd_present;          /* DETD插接状态，0/1。 */
    uint8_t detc_present;          /* DETC插接状态，0/1。 */
    uint8_t u8ParallelPeerDetected;           /* 协议BMR位：发送方检测到并联对端时为1。 */
    uint8_t id_present;            /* ID信号状态，0/1。 */
    uint8_t discharge_mos_on;      /* 放电MOS状态，0/1。 */
    uint8_t charge_mos_on;         /* 充电MOS状态，0/1。 */
    uint8_t precharge_mos_on;      /* 预放MOS状态，0/1。 */
    uint8_t key_present;           /* KEY状态，0/1。 */
    uint8_t parallel_mos_status[2]; /* F218 Data1~2：协议定义的两组并联MOS状态字节。 */
    uint8_t charge_warning_level;  /* F218 Data3：充电告警等级原始值。 */
    uint8_t discharge_warning_level; /* F218 Data4：放电告警等级原始值。 */
    uint8_t work_status;           /* F218 Data5：工作状态原始值。 */

    /* F219：Data0~1电压(0.01V)，Data2~7依次充电/回充/放电限流(0.01A)。 */
    uint16_t charge_voltage_001v;    /* 最大允许充电电压。 */
    uint16_t charge_current_001a;    /* 最大允许充电电流。 */
    uint16_t recharge_current_001a;  /* 最大允许回充电流。 */
    uint16_t discharge_current_001a; /* 最大允许放电电流。 */

    /* F21A/F21B/F21C与F21D/F21E/F21F按PGN顺序保留；不在缓存层翻译位定义。 */
    uint8_t alarm_frame[3][D_CAN_MAX_DATA_LEN]; /* 三个告警PGN，每帧完整8字节。 */
    uint8_t protection_frame[3][D_CAN_MAX_DATA_LEN]; /* 三个保护PGN，每帧完整8字节。 */

    /* F220：欠压值单位0.1V，事件量单位次，剩余充电时间单位分钟；F221为32位位图。 */
    uint16_t controller_uv_0p1v;       /* 控制器欠压值，0.1V分辨率。 */
    uint16_t overcharge_event_count;   /* 过充事件计数。 */
    uint16_t overdischarge_event_count; /* 过放事件计数。 */
    uint16_t charge_remaining_minutes; /* 预计剩余充电时间，单位min。 */
    uint32_t balance_mask;             /* F221均衡状态，Data0为最低有效字节。 */

    /* 以下字段只用于本机槽位：保存业务层故障掩码和AFE故障快照。 */
    /* 对端故障帧不映射到这些业务结构，原始报文保存在前述alarm_frame/protection_frame。 */
    uint16_t local_charge_fault_a;     /* 本机充电故障字A，位定义沿用app_bms_fault业务结构。 */
    uint16_t local_charge_fault_b;     /* 本机充电故障字B，位定义沿用app_bms_fault业务结构。 */
    uint16_t local_discharge_fault_a;  /* 本机放电故障字A，位定义沿用app_bms_fault业务结构。 */
    uint16_t local_discharge_fault_b;  /* 本机放电故障字B，位定义沿用app_bms_fault业务结构。 */
    uint16_t local_general_fault_a;    /* 本机通用故障字A，位定义沿用app_bms_fault业务结构。 */
    uint8_t local_afe_fault_valid;     /* AFE故障快照有效标志；无效时后续AFE位不能用于判断。 */
    uint8_t local_afe_cell_ov;         /* AFE单体过压故障位。 */
    uint8_t local_afe_cell_uv;         /* AFE单体欠压故障位。 */
    uint8_t local_afe_charge_oc1;      /* AFE充电过流一级故障位。 */
    uint8_t local_afe_charge_oc2;      /* AFE充电过流二级故障位。 */
    uint8_t local_afe_discharge_oc1;   /* AFE放电过流一级故障位。 */
    uint8_t local_afe_discharge_oc2;   /* AFE放电过流二级故障位。 */
    uint8_t local_afe_short_circuit;   /* AFE短路故障位。 */
} app_parallel_status_pack_t;

/* 清空全部槽位；CAN初始化时调用一次。并联状态仅在使能宏开启且运行于成品模式时维护。 */
void app_parallel_status_init(void);
/* 成品模式下周期刷新本机快照，并按5秒超时清除离线对端；退出成品模式时清空缓存。 */
void app_parallel_status_task(void);
/* CAN活动/本地唤醒退出ZS静默时调用：平移对端和本机缓存时间戳并请求立即刷新本机数据。 */
void app_parallel_status_resume_after_silence(void);
/* 传入CAN接收帧；仅在成品模式、使能宏开启时处理地址范围内的B1 F20D~F221扩展帧。 */
void app_parallel_status_receive(const t_can_packet *can_packet);

/* 返回当前在线总包数（含已刷新到表中的本机）；宏关闭或非成品模式时返回0。 */
uint8_t app_parallel_status_get_online_count(void);

/* 把指定地址槽复制到调用方缓冲区；复制期间屏蔽并发更新，不暴露可变内部指针。 */
uint8_t app_parallel_status_get_pack_snapshot(uint8_t slot,
                                              app_parallel_status_pack_t *pack);

/* 判断复制出的PGN数据是否在线、曾有效且未超过调用方给出的最大年龄。 */
uint8_t app_parallel_status_pgn_is_fresh(const app_parallel_status_pack_t *pack,
                                         uint8_t pgn_index,
                                         uint32_t max_age_ms);

/* 读取固定连续地址槽位0..14；非成品模式或无效索引时返回NULL。 */
const app_parallel_status_pack_t *app_parallel_status_get_pack(uint8_t slot);

#endif
