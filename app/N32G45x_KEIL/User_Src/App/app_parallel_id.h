#ifndef __APP_PARALLEL_ID_H__
#define __APP_PARALLEL_ID_H__

#include <stdint.h>

#include "App/app_id_assign.h"

/* 并联地址槽位总数：最多15个BMS地址，包含本机和最多14个对端。 */
#define PARALLEL_BMS_MAX_NUM APP_ID_ASSIGN_MAX_PACK_COUNT

/* 并联状态机使用的本机/对端离散输入；每一位表示一个独立的信号或MOS反馈。 */
typedef union
{
    uint8_t u8All;
    struct
    {
        uint8_t u8Detd : 1U;          /* DETD放电插接信号；1表示检测到放电连接。 */
        uint8_t u8Detc : 1U;          /* DETC充电插接信号；1表示检测到充电连接。 */
        uint8_t u8PeerDetected : 1U; /* 并联对端检测状态；1表示至少一个对端在线。 */
        uint8_t u8Id : 1U;            /* 编址标识；本机rank非0时置1。 */
        uint8_t u8DmosState : 1U;     /* DMOS硬件反馈状态；1表示反馈为闭合。 */
        uint8_t u8CmosState : 1U;     /* CMOS硬件反馈状态；1表示反馈为闭合。 */
        uint8_t u8PmosState : 1U;     /* PDMOS硬件反馈状态；1表示反馈为闭合。 */
        uint8_t u8Key : 1U;           /* KEY在位状态；1表示按键/钥匙信号有效。 */
    } tBit;
} tParallelInputSignal;

/*
 * 并联控制所需的单包输入数据。
 * 总压和电流均采用0.01单位：u32TotalVoltage001V的每计数为0.01V，
 * s32Current001A的每计数为0.01A；本工程约定充电为正、放电为负。
 * 故障分级由 app_bms_fault 提供，完整报文状态由 app_parallel_status 缓存；
 * 本结构只保存角色判定和MOS阶段控制直接使用的字段。
 */
typedef struct
{
    tParallelInputSignal tInputSignal;
    uint32_t u32TotalVoltage001V; /* 电池包总压；1 count = 0.01V。 */
    int32_t s32Current001A;       /* 业务滤波电流；1 count = 0.01A，正充电、负放电。 */
} tParallelInputDataGroup;

/* 并联状态机对本机MOS的目标许可；1表示允许该MOS闭合，0表示请求保持关闭。 */
typedef struct
{
    uint8_t u8DmosControl;             /* 放电MOS许可；最终仍由HPM执行安全门控和预放时序。 */
    uint8_t u8CmosControl;             /* 充电MOS许可；HPM仍会应用电流优先和故障门控。 */
    uint8_t u8PmosControl;             /* 预放MOS许可；当前并联阶段机不直接驱动此字段。 */
    uint8_t u8ParallelDischargeState;  /* 预留的放电阶段诊断字段；当前状态机不写入。 */
    uint8_t u8ParallelChargeState;     /* 预留的充电阶段诊断字段；当前状态机不写入。 */
} tParallelOutputDataGroup;

/* 输入数组按CAN连续地址槽位组织；本机输入是该数组中当前编址对应的一项。 */
extern tParallelInputDataGroup gtParallelBmsInput[PARALLEL_BMS_MAX_NUM];
extern tParallelInputDataGroup gtParallelLocalInput;
extern tParallelOutputDataGroup gtParallelLocalOutput;
/* 并联功能总开关；上电初值来自APP_PARAM_PARALLEL_ENABLE，运行时统一由此门控。 */
extern volatile uint8_t g_u8ParallelEnable;
extern uint8_t g_u8ParallelPackCount;
extern uint8_t g_u8VmaxRoleFlag;
extern uint32_t g_u32VoltageDiffMaxMv;

/* 初始化并联阶段计时、输入输出缓存、通信诊断和地址冲突状态。 */
void app_parallel_sig_init(void);

/* 由HPM任务周期调用；内部使用TIM6时间基准，每50ms推进一次并联状态计算。 */
void app_parallel_id_task(void);
/* 总线唤醒后、处理首帧前调用：冻结静默期间的状态机和通信诊断计时。 */
void app_parallel_id_resume_after_silence(void);

/* 更新本机与对端输入、确认电压角色并推进充放电MOS阶段；参数为本次周期毫秒数。 */
void app_parallel_handle(uint16_t u16PeriodMs);

/* 并联SOP放电许可：1表示允许，0表示故障、数据未就绪或压差门控禁止放电。 */
uint8_t app_parallel_cmd_en(void);

/* 并联全局开关开启且处于出货模式时返回1。 */
uint8_t app_parallel_id_is_enabled(void);

/*
 * 由CAN编址调度器报告当前rank是否可用于并联控制。
 * 成员重排、无连接或CAN总线离线时传0；稳定编址完成后传1。
 * UDS/IAP主动暂停编址且保留原rank时不调用本接口，沿用现有rank。
 */
void app_parallel_id_set_assignment_valid(uint8_t u8Valid);

/* 编址完成且本机、所有在线对端控制数据有效时返回1。 */
uint8_t app_parallel_id_is_ready(void);

/* 将当前B1故障快照映射为F218充/放电告警等级：0=正常，1=告警/降额，2=停止。 */
void app_parallel_id_get_warning_levels(uint8_t *pu8ChargeLevel,
                                        uint8_t *pu8DischargeLevel);

/* 曾检测到并联对端后，最后一条对端状态帧超时则返回1；单包初始运行返回0。 */
uint8_t app_parallel_id_get_communication_fail_flag(void);

/* 并联运行时收到源地址与本机相同的B1状态帧，且2秒内仍有重复帧时返回1。 */
uint8_t app_parallel_id_is_address_conflict(void);

/* 在并联状态缓存过滤之前观察CAN帧，用于识别重复的本机BMS源地址。 */
void app_parallel_id_note_can_frame(const t_can_packet *ptCanPacket);

/* F218 Data0 bit2（协议名 BMR）：是否检测到至少一个有效并联对端。 */
uint8_t app_parallel_id_get_peer_detected_flag(void);

/* F218 Data0 bit3：读取本机编址rank是否非0；未完成编址时返回0。 */
uint8_t app_parallel_id_get_id_flag(void);

#endif
