/*
 * GD 7627 并联策略移植到 N32G45x B1 工程。
 *
 * 主要保留GD app_parallel_id.c中的下列行为：
 *   1. 依据所有在线包总压筛选本机是否为最高压/最低压包，并对角色做滤波；
 *   2. 依据总压差、电流方向、DETD/DETC、对端MOS反馈和KEY运行GD充放电阶段机；
 *   3. 并联使能开启但没有检测到有效对端时，仍执行GD原有单包分支；
 *   4. 以GD的200ms调用周期推进1s/2s/5s等延时。
 *
 * 仅边界适配为当前工程的数据源：输入取app_parallel_status快照缓存，
 * 输出作为app_h_power_manage的检测许可输入。实际MOS下发、故障/强制关断、
 * AFE新采样、DETD/DETC安全门控和充电授权继续由现有模块处理。
 * GD的Xingheng告警全局量、BMU/充电互认协议在本工程没有等价信号，故不伪造迁移。
 */
#include "App/app_parallel_id.h"

#include <stdbool.h>
#include <string.h>

#include "App/app_bms_fault.h"
#include "App/app_bms_snapshot.h"
#include "App/app_parallel_status.h"
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "Driver/d_timer.h"
#include "FreeRTOS.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "Protocol/Comm/zs_b1_can_protocol.h"
#include "task.h"

#define TIME_OUT_1S             ((uint16_t)1000U)
#define TIME_OUT_2S             ((uint16_t)2000U)
#define TIME_OUT_5S             ((uint16_t)5000U)
#define APP_PARALLEL_COMM_FAIL_TIMEOUT_MS ((uint32_t)1000UL)
#define APP_PARALLEL_ADDRESS_CONFLICT_RECOVERY_MS ((uint32_t)2000UL)
#define APP_PARALLEL_VDIFF_MV   ((uint32_t)2000UL) /* GD PARALLEL_VDIFF：2000mV，即2V。 */
#define APP_PARALLEL_CURRENT_STATIC_THRESHOLD_001A ((int32_t)500L) /* 5A；负值放电达到阈值以下时暂停2V门控。 */
#define APP_PARALLEL_PERIOD_MS  ((uint32_t)50UL)   /* 本工程每50ms调用一次GD并联状态机。 */
#define APP_PARALLEL_F20D_MAX_AGE_MS ((uint32_t)500UL)
#define APP_PARALLEL_F218_MAX_AGE_MS ((uint32_t)500UL)
#define APP_PARALLEL_F20D_PGN_INDEX  0U
#define APP_PARALLEL_F218_PGN_INDEX  11U

/* GD源代码中充/放电状态机使用的计数器结构和变量名。 */
typedef struct
{
    uint8_t u8State;
    uint8_t u8SinglePackState;
    uint8_t u8Count;
    uint8_t u8Count1;
    uint8_t u8Count2;
    uint8_t u8Count3;
    uint8_t u8Count4;
    uint8_t u8DetcHistory;
    uint8_t u8DetdHistory;
} tParallelHistory;

tParallelInputDataGroup gtParallelBmsInput[PARALLEL_BMS_MAX_NUM];
tParallelInputDataGroup gtParallelLocalInput;
tParallelOutputDataGroup gtParallelLocalOutput;
/* 并联总开关由项目宏初始化；各并联功能通过app_parallel_id_is_enabled统一检查。 */
volatile uint8_t g_u8ParallelEnable = APP_PARAM_PARALLEL_ENABLE;
uint8_t g_u8ParallelPackCount;
uint8_t g_u8VmaxRoleFlag;
uint32_t g_u32VoltageDiffMaxMv;

static tParallelHistory s_tChargeHistory;
static tParallelHistory s_tDischargeHistory;
static uint16_t s_u16ParallelPeriodMs;
static uint32_t s_u32VoltageDiffMinMv;
static uint8_t s_u8VminRoleFlag;
static uint8_t s_u8VmaxConfirmCount;
static uint8_t s_u8VminConfirmCount;
static uint8_t s_u8VmidConfirmCount;
static uint8_t s_u8VoltageDiffFilterCount;
static uint8_t s_u8ChargePackCountHistory;
static uint8_t s_u8DischargePackCountHistory;
static uint8_t s_u8DischargeChargeFlag;
static uint8_t s_u8LocalSlot = 0xFFU;
static uint8_t s_u8RuntimeStarted;
/* CAN通信任务写入；HPM任务读取。无效时并联输出必须保持关闭。 */
static volatile uint8_t s_u8AssignmentValid;
/* 仅由HPM任务维护，用于检测编址失效沿并在本任务上下文清理算法状态。 */
static uint8_t s_u8AssignmentSeen;
/* DETD持续在位且此前已经获准DMOS时，跨临时重新编址保留已闭合许可。 */
static uint8_t s_u8ReaddressDmosHold;
/* 最近一次稳定编址期间DETD是否已在位；用于区分新插入与成员变化重编址。 */
static uint8_t s_u8DetdPresentAtAssignment;
static uint8_t s_u8ControlDataReady;
static uint8_t s_u8PeerDetected;
static uint32_t s_u32LastHandleMs;
static uint8_t s_u8CommandEnabled = 1U;
static uint32_t s_u32CommandBadStartMs;
static uint32_t s_u32CommandGoodStartMs;
/* 通信诊断跨UID重排保留；只在并联策略初始化/退出时清除。 */
static uint8_t s_u8PeerFrameSeen;
static uint8_t s_u8LastPeerRxTimeValid;
static uint8_t s_u8CommunicationFail;
static uint32_t s_u32LastPeerRxMs;
/* CAN接收任务写入，HPM/CAN发送任务读取；时间戳对齐的32位访问为原子读写。 */
static volatile uint8_t s_u8AddressConflictObserved;
static volatile uint32_t s_u32AddressConflictLastRxMs;
/* ZS静默期间冻结并联阶段计时及对端通信超时计时。 */
static uint8_t s_u8SilentHoldActive;
static uint32_t s_u32SilentStartMs;

/*
 * 清除GD状态机计数和当前并联输入/输出。
 * 在运行开关退出、地址切换或输入数据不完整时调用，防止旧状态跨配置沿用。
 */
static void app_parallel_reset_algorithm(void)
{
    memset(gtParallelBmsInput, 0, sizeof(gtParallelBmsInput));
    memset(&gtParallelLocalInput, 0, sizeof(gtParallelLocalInput));
    memset(&gtParallelLocalOutput, 0, sizeof(gtParallelLocalOutput));
    memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
    memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));

    g_u8ParallelPackCount = 0U;
    g_u8VmaxRoleFlag = 0U;
    s_u8VminRoleFlag = 0U;
    g_u32VoltageDiffMaxMv = 0UL;
    s_u32VoltageDiffMinMv = 0UL;
    s_u8VmaxConfirmCount = 0U;
    s_u8VminConfirmCount = 0U;
    s_u8VmidConfirmCount = 0U;
    s_u8VoltageDiffFilterCount = 0U;
    s_u8ChargePackCountHistory = 0U;
    s_u8DischargePackCountHistory = 0U;
    s_u8DischargeChargeFlag = 0U;
    s_u8ControlDataReady = 0U;
    s_u8PeerDetected = 0U;
    s_u8LocalSlot = 0xFFU;
    s_u8CommandEnabled = 1U;
    s_u32CommandBadStartMs = 0UL;
    s_u32CommandGoodStartMs = 0UL;
}

/* 全量重置阶段计时和rank输入时，保留重新编址前已经生效的DMOS许可。 */
static void app_parallel_reset_algorithm_keep_dmos(void)
{
    uint8_t u8KeepDmos = ((s_u8ReaddressDmosHold != 0U) &&
                          (gtParallelLocalOutput.u8DmosControl != 0U)) ? 1U : 0U;

    app_parallel_reset_algorithm();
    gtParallelLocalOutput.u8DmosControl = u8KeepDmos;
}

/* 只在DETD仍在位且无地址冲突时保持重编址前的DMOS许可。 */
static void app_parallel_apply_readdress_dmos_hold(void)
{
    app_bms_snapshot_t tSnapshot;

    app_bms_snapshot_read(&tSnapshot);
    if ((s_u8ReaddressDmosHold != 0U) &&
        (tSnapshot.signal.input.mcu_detd != 0U) &&
        (app_parallel_id_is_address_conflict() == 0U))
    {
        gtParallelLocalOutput.u8DmosControl = 1U;
    }
    else
    {
        s_u8ReaddressDmosHold = 0U;
        if (tSnapshot.signal.input.mcu_detd == 0U)
        {
            s_u8DetdPresentAtAssignment = 0U;
        }
        gtParallelLocalOutput.u8DmosControl = 0U;
    }
}

/* 并联状态机不推进真正静默期；唤醒时把基于TIM6的相关时刻平移同等时长。 */
void app_parallel_id_resume_after_silence(void)
{
    uint32_t u32SilentDurationMs;
    uint32_t u32NowMs = d_timer_get_ms();

    taskENTER_CRITICAL();
    if (s_u8SilentHoldActive == 0U)
    {
        taskEXIT_CRITICAL();
        return;
    }

    u32SilentDurationMs = (uint32_t)(u32NowMs - s_u32SilentStartMs);
    s_u32LastHandleMs += u32SilentDurationMs;
    if (s_u8LastPeerRxTimeValid != 0U)
    {
        s_u32LastPeerRxMs += u32SilentDurationMs;
    }
    if (s_u8AddressConflictObserved != 0U)
    {
        s_u32AddressConflictLastRxMs += u32SilentDurationMs;
    }
    s_u8SilentHoldActive = 0U;
    taskEXIT_CRITICAL();
}

/* 总开关开启且处于出货模式时，并联功能才对外生效。 */
uint8_t app_parallel_id_is_enabled(void)
{
    return ((g_u8ParallelEnable == 1U) &&
            (app_param_get_product_mode() == APP_PARAM_PRODUCT_MODE_SHIPPING)) ?
           1U : 0U;
}

/* 在并联状态缓存过滤前观察B1状态帧；重复源地址沿用GD的2秒无冲突恢复规则。 */
void app_parallel_id_note_can_frame(const t_can_packet *ptCanPacket)
{
    uint32_t u32CanId;
    uint16_t u16Pgn;
    uint8_t u8SourceAddress;

    if ((app_parallel_id_is_enabled() == 0U) ||
        (ptCanPacket == 0) ||
        (ptCanPacket->u32idType != D_CAN_ID_EXTENDED) ||
        (ptCanPacket->u8len != D_CAN_MAX_DATA_LEN))
    {
        return;
    }

    u32CanId = ptCanPacket->u32id & DUCAR_CAN_EXT_ID_MASK;
    u8SourceAddress = (uint8_t)u32CanId;
    u16Pgn = (uint16_t)((((u32CanId >> 16U) & 0xFFUL) << 8U) |
                        ((u32CanId >> 8U) & 0xFFUL));

    if ((u8SourceAddress == g_ducar_can_bms_address) &&
        (u16Pgn >= DUCAR_CAN_PGN_F20D) &&
        (u16Pgn <= DUCAR_CAN_PGN_F221))
    {
        s_u32AddressConflictLastRxMs = d_timer_get_ms();
        s_u8AddressConflictObserved = 1U;
    }
}

/* 地址冲突在最后一条重复源地址帧后连续2秒无新帧时自动恢复。 */
uint8_t app_parallel_id_is_address_conflict(void)
{
    if ((app_parallel_id_is_enabled() == 0U) ||
        (s_u8AddressConflictObserved == 0U))
    {
        return 0U;
    }

    return ((uint32_t)(d_timer_get_ms() - s_u32AddressConflictLastRxMs) <
            APP_PARALLEL_ADDRESS_CONFLICT_RECOVERY_MS) ? 1U : 0U;
}

/*
 * 设置CAN编址是否可安全用于并联计算。
 * 此函数可能在通信任务中调用，不在这里清理算法数组，避免与HPM任务并发写状态；
 * HPM任务观察到0后会自行清状态并保持MOS输出许可关闭。
 */
void app_parallel_id_set_assignment_valid(uint8_t u8Valid)
{
    s_u8AssignmentValid = (u8Valid != 0U) ? 1U : 0U;
}

/*
 * 槽位输入必须同时有有效F20D总压/电流和F218插接/MOS/KEY数据，且未超时。
 * F20E单体极值不参与GD app_parallel_id.c的MOS阶段判定，因此不额外要求它。
 */
static uint8_t app_parallel_pack_is_control_fresh(
    const app_parallel_status_pack_t *ptPack,
    uint32_t u32NowMs)
{
    if ((ptPack == 0) || (ptPack->online == 0U) || (ptPack->total_voltage_001v == 0U))
    {
        return 0U;
    }

    if ((app_parallel_status_pgn_is_fresh(ptPack,
                                           APP_PARALLEL_F20D_PGN_INDEX,
                                           APP_PARALLEL_F20D_MAX_AGE_MS) == 0U) ||
        (app_parallel_status_pgn_is_fresh(ptPack,
                                           APP_PARALLEL_F218_PGN_INDEX,
                                           APP_PARALLEL_F218_MAX_AGE_MS) == 0U))
    {
        return 0U;
    }

    /* u32NowMs由调用方取TIM6时基；保留参数用于显式记录本次快照判断时刻。 */
    (void)u32NowMs;
    return 1U;
}

/*
 * 检查整组对端最近是否仍有状态帧。
 * 未曾收到任何对端帧时保持正常单包行为；曾见过对端后，超过GD的1秒门限置通信诊断，
 * 任一对端恢复报文即清除。该诊断不改变单包MOS回退和故障模块。
 */
static void app_parallel_update_communication_status(uint32_t u32NowMs)
{
    app_parallel_status_pack_t tPack;
    uint8_t u8Slot;
    uint8_t u8LocalSlot;
    uint8_t u8PeerOnline = 0U;
    uint32_t u32NewestPeerRxMs = 0UL;
    uint32_t u32NewestPeerAgeMs = 0xFFFFFFFFUL;

    /* Sleep期间总线静默是协议预期行为，不累计并联对端通信失联时间。 */
    if (zs_b1_can_protocol_is_silent())
    {
        return;
    }

    if (g_ducar_can_bms_address < DUCAR_CAN_BMS_A_ADDRESS)
    {
        u8LocalSlot = 0xFFU;
    }
    else
    {
        u8LocalSlot = (uint8_t)(g_ducar_can_bms_address - DUCAR_CAN_BMS_A_ADDRESS);
    }

    for (u8Slot = 0U; u8Slot < PARALLEL_BMS_MAX_NUM; ++u8Slot)
    {
        if ((u8Slot == u8LocalSlot) ||
            (app_parallel_status_get_pack_snapshot(u8Slot, &tPack) == 0U) ||
            (tPack.online == 0U))
        {
            continue;
        }

        u8PeerOnline = 1U;
        s_u8PeerFrameSeen = 1U;
        /* online即表示last_update_ms有效；0也是TIM6回绕/启动边界上的合法时间戳。 */
        {
            uint32_t u32PeerAgeMs = (uint32_t)(u32NowMs - tPack.last_update_ms);
            if (u32PeerAgeMs < u32NewestPeerAgeMs)
            {
                u32NewestPeerAgeMs = u32PeerAgeMs;
                u32NewestPeerRxMs = tPack.last_update_ms;
            }
        }
    }

    if (u8PeerOnline != 0U)
    {
        /* 更新到当前缓存里最新的一条对端报文时间，处理槽位清除前的离线边界。 */
        if ((u32NewestPeerAgeMs != 0xFFFFFFFFUL) &&
            ((s_u8LastPeerRxTimeValid == 0U) ||
             ((uint32_t)(u32NewestPeerRxMs - s_u32LastPeerRxMs) < 0x80000000UL)))
        {
            s_u32LastPeerRxMs = u32NewestPeerRxMs;
            s_u8LastPeerRxTimeValid = 1U;
        }

        if ((u32NewestPeerAgeMs < APP_PARALLEL_COMM_FAIL_TIMEOUT_MS) &&
            (u32NewestPeerAgeMs != 0xFFFFFFFFUL))
        {
            s_u8CommunicationFail = 0U;
            return;
        }
    }

    if ((s_u8PeerFrameSeen != 0U) &&
        (s_u8LastPeerRxTimeValid != 0U) &&
        ((uint32_t)(u32NowMs - s_u32LastPeerRxMs) >=
         APP_PARALLEL_COMM_FAIL_TIMEOUT_MS))
    {
        s_u8CommunicationFail = 1U;
    }
}

/*
 * 从最终故障快照识别会禁止充/放电的公共保护。
 * 这里只读已有故障位，不写入故障模块；具体方向MOS失效在方向分级函数中单独处理。
 */
static uint8_t app_parallel_fault_has_common_stop(const t_general_fault_A *ptFault)
{
    if ((ptFault->b1AFE_fault != 0U) ||
        (ptFault->b1cell_voltage_diff_fault != 0U) ||
        (ptFault->b1cell_voltage_diff_fault2 != 0U) ||
        (ptFault->b1MOS_OTP != 0U) ||
        (ptFault->b1hardware_second_pro != 0U) ||
        (ptFault->b1SCP_FUSE_fusing != 0U) ||
        (ptFault->b1MOS_OTP_FUSE != 0U) ||
        (ptFault->b1NTC_fault != 0U))
    {
        return 1U;
    }

    return 0U;
}

/* 从B1现有故障位映射GD的充电等级；FCC锁存不作为充电故障。 */
static uint8_t app_parallel_charge_warning_level(const t_bms_fault_flag *ptFault)
{
    const t_charge_fault_A *ptChargeA = &ptFault->ucharge_fault_A.tcharge_fault_A;
    const t_charge_fault_B *ptChargeB = &ptFault->ucharge_fault_B.tcharge_fault_B;
    const t_general_fault_A *ptGeneral = &ptFault->ugeneral_fault_A.tgeneral_fault_A;

    if ((ptChargeA->b1cell_pro_cov != 0U) ||
        (ptChargeA->b1cell_pro_cov2 != 0U) ||
        (ptChargeA->b1pack_pro_otc != 0U) ||
        (ptChargeA->b1pack_pro_utc != 0U) ||
        (ptChargeA->b1pack_pro_occ1 != 0U) ||
        (ptChargeA->b1pack_pro_occ2 != 0U) ||
        (ptChargeA->b1pack_pro_orcc != 0U) ||
        (ptChargeB->b1pack_pro_pov != 0U) ||
        (ptChargeB->b1pack_occ_lock != 0U) ||
        (ptGeneral->b1CHG_MOS_failure != 0U) ||
        (app_parallel_fault_has_common_stop(ptGeneral) != 0U))
    {
        return 2U;
    }

    if ((ptChargeA->b1cell_war_cov != 0U) ||
        (ptChargeA->b1pack_war_otc != 0U) ||
        (ptChargeA->b1pack_war_utc != 0U) ||
        (ptChargeA->b1pack_war_occ != 0U) ||
        (ptChargeA->b1pack_war_orcc != 0U) ||
        (ptChargeB->b1pack_war_pov != 0U) ||
        (ptGeneral->b1cell_voltage_diff_waring != 0U) ||
        (ptGeneral->b1MOS_OTP_waring != 0U))
    {
        return 1U;
    }

    return 0U;
}

/* 从B1现有故障位映射GD的放电等级。 */
static uint8_t app_parallel_discharge_warning_level(const t_bms_fault_flag *ptFault)
{
    const t_discharge_fault_A *ptDischargeA = &ptFault->udischarge_fault_A.tdischarge_fault_A;
    const t_discharge_fault_B *ptDischargeB = &ptFault->udischarge_fault_B.tdischarge_fault_B;
    const t_general_fault_A *ptGeneral = &ptFault->ugeneral_fault_A.tgeneral_fault_A;

    if ((ptDischargeA->b1cell_pro_cuv1 != 0U) ||
        (ptDischargeA->b1cell_pro_cuv2 != 0U) ||
        (ptDischargeA->b1cell_deep_cuv != 0U) ||
        (ptDischargeA->b1pack_pro_otd != 0U) ||
        (ptDischargeA->b1pack_pro_utd != 0U) ||
        (ptDischargeA->b1pack_ocd_lock != 0U) ||
        (ptDischargeB->b1pack_pro_cuv != 0U) ||
        (ptDischargeB->b1pack_pro_ocd1 != 0U) ||
        (ptDischargeB->b1pack_pro_ocd2 != 0U) ||
        (ptDischargeB->b1pack_pro_ocd3 != 0U) ||
        (ptDischargeB->b1pack_pro_ocd4 != 0U) ||
        (ptDischargeB->b1pack_shrot != 0U) ||
        (ptDischargeB->b1predischarge_fault != 0U) ||
        (ptDischargeB->b1predischarge_lock != 0U) ||
        (ptGeneral->b1DSG_MOS_faulure != 0U) ||
        (ptGeneral->b1OTD_FUSE != 0U) ||
        (app_parallel_fault_has_common_stop(ptGeneral) != 0U))
    {
        return 2U;
    }

    if ((ptDischargeA->b1cell_war_cuv != 0U) ||
        (ptDischargeA->b1pack_war_otd != 0U) ||
        (ptDischargeA->b1pack_war_utd != 0U) ||
        (ptDischargeA->b1pack_war_ocd != 0U) ||
        (ptDischargeA->b1low_soc != 0U) ||
        (ptDischargeB->b1pack_war_cuv != 0U) ||
        (ptGeneral->b1cell_voltage_diff_waring != 0U) ||
        (ptGeneral->b1MOS_OTP_waring != 0U))
    {
        return 1U;
    }

    return 0U;
}

/*
 * 把B1共享状态表映射回GD算法输入数组。
 * 地址槽位仍按GD规则使用 0x16 + u8Rank；不重新计算UID排名，也不访问GD私有全局量。
 */
static void app_parallel_inf_update(void)
{
    app_parallel_status_pack_t tPack;
    uint8_t u8Slot;
    uint8_t u8LocalSlot;
    uint8_t u8ValidPackCount = 0U;
    uint8_t u8PeerDetected = 0U;
    uint8_t u8PeerIncomplete = 0U;
    uint32_t u32NowMs = d_timer_get_ms();

    u8LocalSlot = (g_ducar_can_bms_address >= DUCAR_CAN_BMS_A_ADDRESS) ?
                 (uint8_t)(g_ducar_can_bms_address - DUCAR_CAN_BMS_A_ADDRESS) :
                 0xFFU;
    if (u8LocalSlot >= PARALLEL_BMS_MAX_NUM)
    {
        s_u8ReaddressDmosHold = 0U;
        s_u8ControlDataReady = 0U;
        s_u8PeerDetected = 0U;
        memset(&gtParallelLocalInput, 0, sizeof(gtParallelLocalInput));
        memset(&gtParallelLocalOutput, 0, sizeof(gtParallelLocalOutput));
        return;
    }

    if ((s_u8LocalSlot != 0xFFU) && (s_u8LocalSlot != u8LocalSlot))
    {
        /* rank变化时重置阶段计时和成员输入，但保留已确认的重编址DMOS许可。 */
        app_parallel_reset_algorithm_keep_dmos();
    }
    s_u8LocalSlot = u8LocalSlot;
    memset(gtParallelBmsInput, 0, sizeof(gtParallelBmsInput));

    /* 先判断本机槽位；本机数据缺失时不允许运行GD状态机。 */
    if ((app_parallel_status_get_pack_snapshot(u8LocalSlot, &tPack) == 0U) ||
        (app_parallel_pack_is_control_fresh(&tPack, u32NowMs) == 0U))
    {
        s_u8ControlDataReady = 0U;
        s_u8PeerDetected = 0U;
        memset(&gtParallelLocalInput, 0, sizeof(gtParallelLocalInput));
        gtParallelLocalOutput.u8CmosControl = 0U;
        gtParallelLocalOutput.u8PmosControl = 0U;
        app_parallel_apply_readdress_dmos_hold();
        memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
        g_u8ParallelPackCount = 0U;
        return;
    }

    for (u8Slot = 0U; u8Slot < PARALLEL_BMS_MAX_NUM; ++u8Slot)
    {
        if (app_parallel_status_get_pack_snapshot(u8Slot, &tPack) == 0U)
        {
            continue;
        }

        if (u8Slot != u8LocalSlot)
        {
            /* 任一在线对端都表示已检测到并联对端；关键数据未齐时保留该标志并禁止闭合输出。 */
            if (tPack.online != 0U)
            {
                u8PeerDetected = 1U;
                if (app_parallel_pack_is_control_fresh(&tPack, u32NowMs) == 0U)
                {
                    u8PeerIncomplete = 1U;
                    continue;
                }
            }
            else
            {
                continue;
            }
        }

        if (app_parallel_pack_is_control_fresh(&tPack, u32NowMs) == 0U)
        {
            continue;
        }

        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8Detd = tPack.detd_present;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8Detc = tPack.detc_present;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8PeerDetected = tPack.u8ParallelPeerDetected;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8Id = tPack.id_present;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8DmosState = tPack.discharge_mos_on;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8CmosState = tPack.charge_mos_on;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8PmosState = tPack.precharge_mos_on;
        gtParallelBmsInput[u8Slot].tInputSignal.tBit.u8Key = tPack.key_present;
        gtParallelBmsInput[u8Slot].u32TotalVoltage001V = tPack.total_voltage_001v;
        gtParallelBmsInput[u8Slot].s32Current001A = tPack.current_001a;
        if (gtParallelBmsInput[u8Slot].u32TotalVoltage001V != 0UL)
        {
            ++u8ValidPackCount;
        }
    }

    /* 该标志对应GD协议中的BMR位，语义为检测到并联通信；B1依据本机槽以外的有效CAN包推导。 */
    gtParallelBmsInput[u8LocalSlot].tInputSignal.tBit.u8PeerDetected = u8PeerDetected;
    gtParallelBmsInput[u8LocalSlot].tInputSignal.tBit.u8Id =
        (uint8_t)((u8LocalSlot != 0U) ? 1U : 0U);

    gtParallelLocalInput = gtParallelBmsInput[u8LocalSlot];
    g_u8ParallelPackCount = u8ValidPackCount;
    s_u8PeerDetected = u8PeerDetected;
    s_u8ControlDataReady = (uint8_t)((u8ValidPackCount != 0U) &&
                                     (u8PeerIncomplete == 0U));
}

/*
 * GD原始最高/最低压包筛选和压差计算。
 * u32TotalVoltage001V每计数为0.01V；与GD一致，乘10后得到mV，再和2000mV门限比较。
 */
static void app_parallel_voltage_role_update(void)
{
    uint8_t u8Index;
    uint8_t u8LocalSlot;
    uint8_t u8VmaxSlot;
    uint8_t u8VminSlot;
    uint8_t u8AllCurrentStatic = 1U;
    uint32_t u32VmaxVoltage001V;
    uint32_t u32VminVoltage001V;

    if ((g_u8ParallelPackCount == 0U) || (s_u8LocalSlot >= PARALLEL_BMS_MAX_NUM))
    {
        g_u8VmaxRoleFlag = 0U;
        s_u8VminRoleFlag = 0U;
        g_u32VoltageDiffMaxMv = 0UL;
        s_u32VoltageDiffMinMv = 0UL;
        return;
    }

    u8LocalSlot = s_u8LocalSlot;
    u8VmaxSlot = u8LocalSlot;
    u8VminSlot = u8LocalSlot;
    u32VmaxVoltage001V = gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V;
    u32VminVoltage001V = gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V;

    for (u8Index = 0U; u8Index < PARALLEL_BMS_MAX_NUM; ++u8Index)
    {
        uint32_t u32Voltage001V = gtParallelBmsInput[u8Index].u32TotalVoltage001V;

        if (u32Voltage001V == 0UL)
        {
            continue;
        }
        if (u32Voltage001V > u32VmaxVoltage001V)
        {
            u32VmaxVoltage001V = u32Voltage001V;
            u8VmaxSlot = u8Index;
        }
        if (u32Voltage001V < u32VminVoltage001V)
        {
            u32VminVoltage001V = u32Voltage001V;
            u8VminSlot = u8Index;
        }
        if (gtParallelBmsInput[u8Index].s32Current001A < -APP_PARALLEL_CURRENT_STATIC_THRESHOLD_001A)
        {
            /* 压差更新条件：没有任何包正在以超过5A电流放电。 */
            u8AllCurrentStatic = 0U;
        }
    }

    /* 保留GD VFILTER_TIME=4及“超过阈值”后确认角色的计数行为。 */
    if (u8VmaxSlot == u8LocalSlot)
    {
        if (++s_u8VmaxConfirmCount > 4U)
        {
            s_u8VmaxConfirmCount = 0U;
            g_u8VmaxRoleFlag = 1U;
            s_u8VminRoleFlag = 0U;
        }
        s_u8VminConfirmCount = 0U;
        s_u8VmidConfirmCount = 0U;
    }
    else if (u8VminSlot == u8LocalSlot)
    {
        if (++s_u8VminConfirmCount > 4U)
        {
            s_u8VminConfirmCount = 0U;
            s_u8VminRoleFlag = 1U;
            g_u8VmaxRoleFlag = 0U;
        }
        s_u8VmaxConfirmCount = 0U;
        s_u8VmidConfirmCount = 0U;
    }
    else
    {
        if (++s_u8VmidConfirmCount > 4U)
        {
            s_u8VmidConfirmCount = 0U;
            g_u8VmaxRoleFlag = 0U;
            s_u8VminRoleFlag = 0U;
        }
        s_u8VmaxConfirmCount = 0U;
        s_u8VminConfirmCount = 0U;
    }

    if (u8AllCurrentStatic != 0U)
    {
        if (++s_u8VoltageDiffFilterCount > 4U)
        {
            s_u8VoltageDiffFilterCount = 4U;
            g_u32VoltageDiffMaxMv = (u32VmaxVoltage001V > gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V) ?
                (u32VmaxVoltage001V - gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V) * 10UL : 0UL;
            s_u32VoltageDiffMinMv = (gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V > u32VminVoltage001V) ?
                (gtParallelBmsInput[u8LocalSlot].u32TotalVoltage001V - u32VminVoltage001V) * 10UL : 0UL;
        }
    }
    else
    {
        s_u8VoltageDiffFilterCount = 0U;
    }
}

/* GD原始充电MOS阶段状态机。 */
static void chg_output_cal(void)
{
    uint8_t u8ParallelDmosFeedback = 0U;
    uint8_t u8ParallelChargeCurrentDetected = 0U;
    uint8_t u8Index;

    if (s_u8ChargePackCountHistory != g_u8ParallelPackCount)
    {
        s_u8ChargePackCountHistory = g_u8ParallelPackCount;
        memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
    }

    /* 与GD一致：检查其它包DMOS反馈以及正向充电电流。 */
    for (u8Index = 0U; u8Index < PARALLEL_BMS_MAX_NUM; ++u8Index)
    {
        if ((gtParallelBmsInput[u8Index].tInputSignal.tBit.u8DmosState != 0U) &&
            (u8Index != s_u8LocalSlot))
        {
            u8ParallelDmosFeedback = 1U;
            break;
        }
        else
        {
            u8ParallelDmosFeedback = 0U;
        }
        if ((gtParallelBmsInput[u8Index].s32Current001A > 100L) && (u8Index != s_u8LocalSlot))
        {
            u8ParallelChargeCurrentDetected = 1U;
            break;
        }
        else
        {
            u8ParallelChargeCurrentDetected = 0U;
        }
    }

    if (gtParallelLocalInput.tInputSignal.tBit.u8PeerDetected != 0U)
    {
        switch (s_tChargeHistory.u8State)
        {
            case 0U:
                /* GD原逻辑：此阶段不主动改变MOS，等待1秒。 */
                if (s_tChargeHistory.u8Count++ >
                    (uint8_t)(TIME_OUT_1S / s_u16ParallelPeriodMs))
                {
                    s_tChargeHistory.u8State = 1U;
                    s_tChargeHistory.u8Count = 0U;
                }
                break;

            case 1U:
                if (s_u8VminRoleFlag != 0U)
                {
                    if (s_tChargeHistory.u8Count++ >
                        (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                    {
                        s_tChargeHistory.u8State = 2U;
                        s_tChargeHistory.u8Count = 0U;
                    }
                }
                else if (s_tChargeHistory.u8Count1++ >
                         (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                {
                    s_tChargeHistory.u8State = 4U;
                    s_tChargeHistory.u8Count1 = 0U;
                }
                break;

            case 2U:
                gtParallelLocalOutput.u8CmosControl = 1U;
                if ((gtParallelLocalInput.s32Current001A > 100L) &&
                    (gtParallelLocalOutput.u8DmosControl == 0U))
                {
                    s_tChargeHistory.u8State = 3U;
                }
                if ((gtParallelLocalInput.s32Current001A < -100L) &&
                    (u8ParallelChargeCurrentDetected != 0U) && (u8ParallelDmosFeedback != 0U))
                {
                    if (s_tChargeHistory.u8Count++ >
                        (uint8_t)(TIME_OUT_5S / s_u16ParallelPeriodMs))
                    {
                        gtParallelLocalOutput.u8DmosControl = 0U;
                        s_tChargeHistory.u8State = 2U;
                        s_tChargeHistory.u8Count = 0U;
                    }
                }
                else
                {
                    s_tChargeHistory.u8Count = 0U;
                }
                break;

            case 3U:
                gtParallelLocalOutput.u8DmosControl = 1U;
                gtParallelLocalOutput.u8CmosControl = 1U;
                if ((gtParallelLocalInput.s32Current001A < -100L) &&
                    (u8ParallelChargeCurrentDetected != 0U) && (u8ParallelDmosFeedback != 0U))
                {
                    if (s_tChargeHistory.u8Count++ >
                        (uint8_t)(TIME_OUT_5S / s_u16ParallelPeriodMs))
                    {
                        gtParallelLocalOutput.u8DmosControl = 0U;
                        s_tChargeHistory.u8State = 2U;
                        s_tChargeHistory.u8Count = 0U;
                    }
                }
                else
                {
                    s_tChargeHistory.u8Count = 0U;
                }
                break;

            case 4U:
                if (s_u32VoltageDiffMinMv > APP_PARALLEL_VDIFF_MV)
                {
                    if (u8ParallelDmosFeedback != 0U)
                    {
                        gtParallelLocalOutput.u8DmosControl = 0U;
                        s_tChargeHistory.u8State = 2U;
                    }
                }
                else
                {
                    s_tChargeHistory.u8State = 2U;
                }
                break;

            default:
                break;
        }
    }
    else
    {
        /* GD未检测到并联对端（单包）充电分支：直接允许充电MOS和放电MOS。 */
        gtParallelLocalOutput.u8CmosControl = 1U;
        gtParallelLocalOutput.u8DmosControl = 1U;
    }
}

/* GD原始放电MOS阶段状态机。 */
static void dsg_output_cal(void)
{
    uint8_t u8ParallelDischargeCurrentDetected = 0U;
    uint8_t u8Index;

    if (s_u8DischargePackCountHistory != g_u8ParallelPackCount)
    {
        s_u8DischargePackCountHistory = g_u8ParallelPackCount;
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
        s_u8DischargeChargeFlag = 0U;
    }

    for (u8Index = 0U; u8Index < PARALLEL_BMS_MAX_NUM; ++u8Index)
    {
        if ((gtParallelBmsInput[u8Index].s32Current001A < 0L) && (u8Index != s_u8LocalSlot))
        {
            u8ParallelDischargeCurrentDetected = 1U;
            break;
        }
        else
        {
            u8ParallelDischargeCurrentDetected = 0U;
        }
    }

    if (gtParallelLocalInput.tInputSignal.tBit.u8Detd != 0U)
    {
        if (gtParallelLocalInput.tInputSignal.tBit.u8PeerDetected != 0U)
        {
            switch (s_tDischargeHistory.u8State)
            {
                case 0U:
                    gtParallelLocalOutput.u8CmosControl = 0U;
                    if (s_tDischargeHistory.u8Count++ >
                        (uint8_t)(TIME_OUT_1S / s_u16ParallelPeriodMs))
                    {
                        s_tDischargeHistory.u8State = 1U;
                        s_tDischargeHistory.u8Count = 0U;
                    }
                    break;

                case 1U:
                    gtParallelLocalOutput.u8DmosControl = 1U;
                    gtParallelLocalOutput.u8CmosControl = 0U;
                    if (g_u8VmaxRoleFlag != 0U)
                    {
                        if (s_tDischargeHistory.u8Count++ >
                            (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                        {
                            s_tDischargeHistory.u8State = 2U;
                            s_tDischargeHistory.u8Count = 0U;
                        }
                    }
                    else if (s_tDischargeHistory.u8Count1++ >
                             (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                    {
                        s_tDischargeHistory.u8State = 3U;
                        s_tDischargeHistory.u8Count1 = 0U;
                    }
                    break;

                case 2U:
                    gtParallelLocalOutput.u8DmosControl = 1U;
                    gtParallelLocalOutput.u8CmosControl = 1U;
                    if ((gtParallelLocalInput.s32Current001A > 2000L) && (u8ParallelDischargeCurrentDetected != 0U))
                    {
                        if (s_tDischargeHistory.u8Count++ >
                            (uint8_t)(TIME_OUT_1S / s_u16ParallelPeriodMs))
                        {
                            s_u8DischargeChargeFlag = 1U;
                            s_tDischargeHistory.u8State = 1U;
                            s_tDischargeHistory.u8Count = 0U;
                        }
                    }
                    else if ((gtParallelLocalInput.s32Current001A > 500L) && (u8ParallelDischargeCurrentDetected != 0U))
                    {
                        if (s_tDischargeHistory.u8Count1++ >
                            (uint8_t)(TIME_OUT_5S / s_u16ParallelPeriodMs))
                        {
                            s_u8DischargeChargeFlag = 1U;
                            s_tDischargeHistory.u8State = 1U;
                            s_tDischargeHistory.u8Count1 = 0U;
                        }
                    }
                    else if (g_u8VmaxRoleFlag == 0U)
                    {
                        if (s_tDischargeHistory.u8Count2++ >
                            (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                        {
                            s_tDischargeHistory.u8State = 3U;
                            s_tDischargeHistory.u8Count2 = 0U;
                        }
                    }
                    break;

                case 3U:
                    if ((g_u32VoltageDiffMaxMv > APP_PARALLEL_VDIFF_MV) ||
                        (s_u8DischargeChargeFlag != 0U))
                    {
                        s_tDischargeHistory.u8State =
                            (gtParallelLocalInput.s32Current001A < -100L) ? 2U : 1U;
                    }
                    else
                    {
                        s_tDischargeHistory.u8State = 2U;
                    }
                    break;

                default:
                    break;
            }
        }
        else
        {
            /* GD未检测到并联对端（单包）放电分支：电流或KEY触发2秒后开放CMOS。 */
            switch (s_tDischargeHistory.u8SinglePackState)
            {
                case 0U:
                    gtParallelLocalOutput.u8DmosControl = 1U;
                    gtParallelLocalOutput.u8CmosControl = 0U;
                    if ((gtParallelLocalInput.s32Current001A < -100L) ||
                        (gtParallelLocalInput.tInputSignal.tBit.u8Key != 0U))
                    {
                        if (s_tDischargeHistory.u8Count++ >
                            (uint8_t)(TIME_OUT_2S / s_u16ParallelPeriodMs))
                        {
                            s_tDischargeHistory.u8SinglePackState = 1U;
                            s_tDischargeHistory.u8Count = 0U;
                        }
                    }
                    else
                    {
                        s_tDischargeHistory.u8Count = 0U;
                    }
                    break;

                case 1U:
                    gtParallelLocalOutput.u8DmosControl = 1U;
                    gtParallelLocalOutput.u8CmosControl = 1U;
                    break;

                default:
                    break;
            }
        }
    }
    else
    {
        gtParallelLocalOutput.u8DmosControl = 0U;
        gtParallelLocalOutput.u8CmosControl = 0U;
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
    }
}

/* GD原始的充电/放电状态机选择：DETC优先进入充电分支。 */
static void app_parallel_output_calculation(void)
{
    if (gtParallelLocalInput.tInputSignal.tBit.u8Detc != 0U)
    {
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
        chg_output_cal();
    }
    else
    {
        memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
        dsg_output_cal();
    }
}

/* GD的独立30秒充电互认来自GD专有车辆码/充电器报文，本工程授权由MOS管理器负责。 */
static void app_parallel_output_send(void)
{
    /* 保留GD周期入口；当前B1没有可一一映射的Xingheng充电互认信号。 */
}

/* GD原SOP放电许可逻辑，使用本工程的实时故障快照替代GD专有报警全局量。 */
uint8_t app_parallel_cmd_en(void)
{
    struct _app_bms_fault_t fault;
    uint8_t u8Index;
    uint8_t u8CurrentStatic = 1U;
    uint32_t u32NowMs;

    if (app_parallel_id_is_enabled() == 0U)
    {
        return 1U;
    }
    if ((s_u8ControlDataReady == 0U) || (s_u16ParallelPeriodMs == 0U))
    {
        return 0U;
    }

    app_bms_fault_snapshot_read(&fault);
    if ((fault.tbms_fault_stat.u8discharge_pro_flag != 0U) ||
        (fault.tbms_fault_stat.u8general_fault_flag != 0U) ||
        (fault.tbms_fault_stat.u8PF_fault_flag != 0U))
    {
        return 0U;
    }

    /* 每次重新计算cur_static；修复GD原文件中static标志一旦变0便不会恢复的问题。 */
    for (u8Index = 0U; u8Index < PARALLEL_BMS_MAX_NUM; ++u8Index)
    {
        if ((gtParallelBmsInput[u8Index].u32TotalVoltage001V != 0UL) &&
            (gtParallelBmsInput[u8Index].s32Current001A < -APP_PARALLEL_CURRENT_STATIC_THRESHOLD_001A))
        {
            u8CurrentStatic = 0U;
            break;
        }
    }

    if ((gtParallelLocalInput.tInputSignal.tBit.u8Detd == 0U) ||
        (gtParallelLocalInput.tInputSignal.tBit.u8PeerDetected == 0U))
    {
        return 1U;
    }

    if (u8CurrentStatic != 0U)
    {
        u32NowMs = d_timer_get_ms();
        if ((g_u32VoltageDiffMaxMv > APP_PARALLEL_VDIFF_MV) &&
            (g_u8VmaxRoleFlag == 0U))
        {
            s_u32CommandGoodStartMs = 0UL;
            if (s_u32CommandBadStartMs == 0UL)
            {
                s_u32CommandBadStartMs = u32NowMs;
            }
            else if ((uint32_t)(u32NowMs - s_u32CommandBadStartMs) >= TIME_OUT_2S)
            {
                s_u8CommandEnabled = 0U;
                s_u32CommandBadStartMs = u32NowMs;
            }
        }
        else
        {
            s_u32CommandBadStartMs = 0UL;
        }

        if ((g_u32VoltageDiffMaxMv < APP_PARALLEL_VDIFF_MV) &&
            (g_u8VmaxRoleFlag == 0U))
        {
            s_u32CommandBadStartMs = 0UL;
            if (s_u32CommandGoodStartMs == 0UL)
            {
                s_u32CommandGoodStartMs = u32NowMs;
            }
            else if ((uint32_t)(u32NowMs - s_u32CommandGoodStartMs) >= TIME_OUT_2S)
            {
                s_u8CommandEnabled = 1U;
                s_u32CommandGoodStartMs = u32NowMs;
            }
        }
        else
        {
            s_u32CommandGoodStartMs = 0UL;
        }
    }

    return s_u8CommandEnabled;
}

/* 对外保留GD源代码的初始化入口名称。 */
void app_parallel_sig_init(void)
{
    app_parallel_reset_algorithm();
    s_u16ParallelPeriodMs = 0U;
    s_u8RuntimeStarted = 0U;
    s_u8AssignmentSeen = 0U;
    s_u8ReaddressDmosHold = 0U;
    s_u8DetdPresentAtAssignment = 0U;
    s_u8SilentHoldActive = 0U;
    s_u32SilentStartMs = 0UL;
    s_u32LastHandleMs = 0UL;
    s_u8PeerFrameSeen = 0U;
    s_u8LastPeerRxTimeValid = 0U;
    s_u8CommunicationFail = 0U;
    s_u32LastPeerRxMs = 0UL;
    s_u8AddressConflictObserved = 0U;
    s_u32AddressConflictLastRxMs = 0UL;
}

/* GD源代码周期入口：先刷新输入，再算角色/压差，再推进MOS阶段。 */
void app_parallel_handle(uint16_t u16PeriodMs)
{
    if (u16PeriodMs == 0U)
    {
        gtParallelLocalOutput.u8DmosControl = 0U;
        gtParallelLocalOutput.u8CmosControl = 0U;
        s_u8ControlDataReady = 0U;
        return;
    }

    s_u16ParallelPeriodMs = u16PeriodMs;
    app_parallel_inf_update();
    if (s_u8ControlDataReady == 0U)
    {
        /* 有对端在线但关键状态缺失/超时时，不能照GD单包分支自动闭合MOS。 */
        gtParallelLocalOutput.u8CmosControl = 0U;
        gtParallelLocalOutput.u8PmosControl = 0U;
        app_parallel_apply_readdress_dmos_hold();
        memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
        return;
    }

    app_parallel_voltage_role_update();
    app_parallel_output_calculation();
    /* 首次使用新成员数据计算完成后，由并联阶段机接管DMOS许可。 */
    s_u8ReaddressDmosHold = 0U;
    app_parallel_output_send();
}

/*
 * 10ms HPM任务调用。GD main.c每200ms调用一次handle，因此此处使用TIM6时基
 * 维持同样的算法节拍，而不是将GD以200ms为单位的计数器误按10ms运行。
 */
void app_parallel_id_task(void)
{
    uint32_t u32NowMs;

    if (app_parallel_id_is_enabled() == 0U)
    {
        if (s_u8RuntimeStarted != 0U)
        {
            app_parallel_sig_init();
        }
        return;
    }

    if (s_u8RuntimeStarted == 0U)
    {
        /* 静态变量已零初始化；不要在首次HPM调度时清掉CAN接收任务已捕获的地址冲突帧。 */
        s_u8RuntimeStarted = 1U;
        s_u32LastHandleMs = d_timer_get_ms();
    }

    u32NowMs = d_timer_get_ms();
    if (zs_b1_can_protocol_is_silent())
    {
        taskENTER_CRITICAL();
        if (s_u8SilentHoldActive == 0U)
        {
            s_u8SilentHoldActive = 1U;
            s_u32SilentStartMs = u32NowMs;
        }
        taskEXIT_CRITICAL();
        return;
    }
    app_parallel_id_resume_after_silence();
    u32NowMs = d_timer_get_ms();

    app_parallel_update_communication_status(u32NowMs);

    if (s_u8AssignmentValid == 0U)
    {
        /*
         * UID尚未完成或成员正在重排：重置rank相关阶段计时和成员输入。
         * 仅当此前已有DMOS许可且DETD持续在位时保留该许可；首次接入不预先闭合。
         * 清理留在HPM任务内执行，避免通信任务与并联状态机同时写全局数组。
         */
        if (s_u8AssignmentSeen != 0U)
        {
            app_bms_snapshot_t tSnapshot;

            app_bms_snapshot_read(&tSnapshot);
            s_u8ReaddressDmosHold =
                ((s_u8DetdPresentAtAssignment != 0U) &&
                 (tSnapshot.signal.input.mcu_detd != 0U) &&
                 (gtParallelLocalOutput.u8DmosControl != 0U) &&
                 (app_parallel_id_is_address_conflict() == 0U)) ? 1U : 0U;
            if (tSnapshot.signal.input.mcu_detd == 0U)
            {
                s_u8DetdPresentAtAssignment = 0U;
            }
            app_parallel_reset_algorithm_keep_dmos();
            s_u8AssignmentSeen = 0U;
        }
        app_parallel_apply_readdress_dmos_hold();
        gtParallelLocalOutput.u8CmosControl = 0U;
        gtParallelLocalOutput.u8PmosControl = 0U;
        s_u8ControlDataReady = 0U;
        s_u8PeerDetected = 0U;
        return;
    }
    s_u8AssignmentSeen = 1U;
    {
        app_bms_snapshot_t tSnapshot;
        app_bms_snapshot_read(&tSnapshot);
        s_u8DetdPresentAtAssignment =
            (tSnapshot.signal.input.mcu_detd != 0U) ? 1U : 0U;
    }

    if (app_parallel_id_is_address_conflict() != 0U)
    {
        /* 地址重复时并联数据源不可区分，按GD冲突路径撤销所有并联MOS许可。 */
        s_u8ReaddressDmosHold = 0U;
        gtParallelLocalOutput.u8DmosControl = 0U;
        gtParallelLocalOutput.u8CmosControl = 0U;
        gtParallelLocalOutput.u8PmosControl = 0U;
        s_u8ControlDataReady = 0U;
        memset(&s_tChargeHistory, 0, sizeof(s_tChargeHistory));
        memset(&s_tDischargeHistory, 0, sizeof(s_tDischargeHistory));
        return;
    }

    if ((uint32_t)(u32NowMs - s_u32LastHandleMs) >= APP_PARALLEL_PERIOD_MS)
    {
        s_u32LastHandleMs = u32NowMs;
        app_parallel_handle((uint16_t)APP_PARALLEL_PERIOD_MS);
    }
}

uint8_t app_parallel_id_is_ready(void)
{
    return ((s_u8AssignmentValid != 0U) &&
            (s_u8ControlDataReady != 0U) &&
            (app_parallel_id_is_address_conflict() == 0U)) ? 1U : 0U;
}

/* 一次读取同一份最终故障快照，分别输出F218充电和放电告警等级。 */
void app_parallel_id_get_warning_levels(uint8_t *pu8ChargeLevel,
                                        uint8_t *pu8DischargeLevel)
{
    struct _app_bms_fault_t tFaultSnapshot;

    if (pu8ChargeLevel == 0)
    {
        return;
    }
    if (pu8DischargeLevel == 0)
    {
        *pu8ChargeLevel = 0U;
        return;
    }

    *pu8ChargeLevel = 0U;
    *pu8DischargeLevel = 0U;
    if (app_parallel_id_is_enabled() == 0U)
    {
        return;
    }

    memset(&tFaultSnapshot, 0, sizeof(tFaultSnapshot));
    app_bms_fault_snapshot_read(&tFaultSnapshot);
    *pu8ChargeLevel = app_parallel_charge_warning_level(&tFaultSnapshot.tbms_fault_flag);
    *pu8DischargeLevel = app_parallel_discharge_warning_level(&tFaultSnapshot.tbms_fault_flag);
}

/* 并联通信诊断仅供诊断读取；不会改变单包回退或MOS控制。 */
uint8_t app_parallel_id_get_communication_fail_flag(void)
{
    return ((app_parallel_id_is_enabled() != 0U) &&
            (s_u8CommunicationFail != 0U)) ? 1U : 0U;
}

uint8_t app_parallel_id_get_peer_detected_flag(void)
{
    return ((app_parallel_id_is_enabled() != 0U) &&
            (s_u8AssignmentValid != 0U) && (s_u8PeerDetected != 0U)) ? 1U : 0U;
}

uint8_t app_parallel_id_get_id_flag(void)
{
    return ((app_parallel_id_is_enabled() != 0U) &&
            (s_u8AssignmentValid != 0U) &&
            (g_ducar_can_bms_address != DUCAR_CAN_BMS_A_ADDRESS)) ? 1U : 0U;
}
