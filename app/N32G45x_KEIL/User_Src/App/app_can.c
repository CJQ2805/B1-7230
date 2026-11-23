/*
 * 文件作用：APP层CAN总调度入口。
 *
 * 数据流固定为：
 *   CAN中断 -> d_can接收环形队列 -> CAN_ReceiveTask() -> 协议分发；
 *   各协议发送任务 -> CAN_TransmitTask() -> d_can_send() -> CAN硬件邮箱。
 *
 * 本文件只负责“识别帧属于哪个协议”和“调用对应协议模块”，不在这里
 * 解析普通协议字段，也不在这里实现IAP传输状态机。这样后续增加UDS、
 * 宗申协议时，只需要扩展帧分类和对应接口，不会把不同协议揉在一起。
 */
#include "App/app_can.h"

#include <stdint.h>

#include "Driver/d_can.h"
#include "App/app_param.h"
#include "App/app_bms_obc.h"
#include "App/app_id_assign.h"
#include "App/app_parallel_id.h"
#include "App/app_parallel_status.h"
#include "App/app_input_signals.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "Protocol/Comm/zs_b1_can_protocol.h"
#include "Protocol/IAP/ducar_can_iap_protocol.h"
#include "Protocol/UDS/UDSBase/uds_port.h"
#include "Protocol/UDS/UDSLogic/SID28_CommunicationControl.h"
#if (APP_PARAM_UDS_ENABLE == 1U)
#include "Protocol/UDS/UDSLogic/SID10_SessionControl.h"
#endif
#include "SEGGER_RTT.h"

typedef enum
{
    APP_CAN_ID_NONE = 0U,
    APP_CAN_ID_OBC,
    APP_CAN_ID_ECM_STATUS6,
    APP_CAN_ID_UDS,
    APP_CAN_ID_ASSIGN,
    APP_CAN_ID_DUCAR_NORMAL,
    APP_CAN_ID_DUCAR_IAP
} tAppCanIdType;

#define APP_CAN_ECM_STATUS6_RX_CAN_ID  0x105U
#define APP_CAN_ECM_STATUS6_RX_DLC     8U

/* Comm_Task约1ms调用一次CAN任务；并联编址、状态缓存、普通报文发送均在此调度链内协同。 */
static uint16_t s_u16AppCanTxDiagTicks;
/* STOP唤醒先恢复协议会话；保留标记供恢复后的第一帧按静默唤醒路径处理。 */
static volatile uint8_t s_u8SilentWakePending;

/* 仅在并联已使能、产品处于出货模式且DETD/DETC至少一个在位时启动编址。 */
static bool app_can_id_assign_requested(void)
{
    return (app_parallel_id_is_enabled() != 0U) &&
           ((gt_app_input_signals.mcu_detd != 0U) ||
            (gt_app_input_signals.mcu_detc != 0U));
}

/* 仅真实静默期间保持当前编址；Sleep准备期仍正常运行编址检查。 */
static bool app_can_parallel_silent_hold_active(void)
{
    return ((app_parallel_id_is_enabled() != 0U) &&
            zs_b1_can_protocol_is_silent());
}

/* 升级或UDS非默认会话期间暂停UID编址，避免编址帧占用诊断总线。 */
static bool app_can_id_assign_paused(void)
{
    if (ducar_can_iap_protocol_is_busy())
    {
        return true;
    }
#if (APP_PARAM_UDS_ENABLE == 1U)
    if (get_current_session() != UDS_SESSION_STD)
    {
        return true;
    }
#endif
    return false;
}

/*
 * 推进UID编址并返回普通周期报文是否允许发送。
 * 编址启动/重排、IAP/UDS暂停、协议静默、CAN离线或rank无效时，普通周期帧暂不发送；
 * 编址有效后将基地址加rank写入全局CAN源地址，并通知并联状态机当前rank有效。
 */
static bool app_can_id_assign_update(void)
{
    uint8_t u8Rank;
    uint8_t u8ParallelPackCount;

    if (g_u8ParallelEnable != 1U)
    {
        /* 运行中关闭总开关时撤销旧rank，恢复基础源地址，避免并联残留影响普通通信。 */
        (void)app_id_assign_task(false, 0U);
        g_ducar_can_bms_address = DUCAR_CAN_BMS_A_ADDRESS;
        app_parallel_id_set_assignment_valid(0U);
        return true;
    }

    /*
     * 宗申周期报文已停发的静默窗口内冻结当前地址/rank和编址有效状态。
     * 若诊断会话要求暂停普通发送，只暂时返回不可发送，不调用task(false)清掉rank。
     */
    if (app_can_parallel_silent_hold_active())
    {
        if (app_can_id_assign_paused())
        {
            return false;
        }
        return (id_assign_state == ID_ASSIGNED);
    }

    if (!app_can_id_assign_requested())
    {
        (void)app_id_assign_task(false, 0U);
        g_ducar_can_bms_address = DUCAR_CAN_BMS_A_ADDRESS;
        app_parallel_id_set_assignment_valid(0U);
        return true;
    }

    if (app_can_id_assign_paused())
    {
        /* 保留当前地址给升级会话；恢复普通通信时重新收集UID。 */
        (void)app_id_assign_task(false, 0U);
        return false;
    }

    if (!d_can_is_bus_online(&can0))
    {
        (void)app_id_assign_task(false, 0U);
        app_parallel_id_set_assignment_valid(0U);
        return false;
    }

    /*
     * 并联在线数只在DETD允许B1主动状态帧互通时用于成员变化校验；
     * 其它时刻传0，避免把暂时收不到周期帧误判为并联成员变化。
     */
    u8ParallelPackCount = (gt_app_input_signals.mcu_detd != 0U) ?
                   app_parallel_status_get_online_count() : 0U;
    u8Rank = app_id_assign_task(true, u8ParallelPackCount);
    if ((id_assign_state != ID_ASSIGNED) ||
        (u8Rank >= APP_ID_ASSIGN_MAX_PACK_COUNT))
    {
        /*
         * 正常编址/成员重排期间撤销旧rank的并联控制资格。
         * app_parallel_id模块会在HPM任务内清除旧成员状态并关断并联许可。
         */
        app_parallel_id_set_assignment_valid(0U);
        return false;
    }

    /* 编址完成后用基地址+rank更新全局CAN源地址，状态缓存随task迁移到新槽位。 */
    g_ducar_can_bms_address = (uint8_t)(DUCAR_CAN_BMS_A_ADDRESS + u8Rank);
    app_parallel_id_set_assignment_valid(1U);
    return true;
}

/*
 * 根据帧格式和CAN ID判断协议归属。
 *
 * 标准帧预留给UDS；扩展帧先判断DUCAR CAN IAP，再交给普通DUCAR协议。
 * 这里不判断业务数据是否正确，业务合法性由具体协议模块继续判断。
 */
static tAppCanIdType app_can_id_check(const t_can_packet *ptCanPacket)
{
    if (ptCanPacket == 0)
    {
        return APP_CAN_ID_NONE;
    }

    if (ptCanPacket->u32idType == D_CAN_ID_STANDARD)
    {
        /* 客户OBC故障报文使用标准ID 0x393，优先于UDS标准帧路由。 */
        if (ptCanPacket->u32id == APP_BMS_OBC_RX_CAN_ID)
        {
            return APP_CAN_ID_OBC;
        }

        /* ECM_Status6 0x105为KEY在位心跳，标准帧优先于UDS路由。 */
        if (ptCanPacket->u32id == APP_CAN_ECM_STATUS6_RX_CAN_ID)
        {
            return APP_CAN_ID_ECM_STATUS6;
        }

#if (APP_PARAM_UDS_ENABLE == 1U)
        /* 标准帧只在UDS总开关启用时才交给UDS协议栈。 */
        return APP_CAN_ID_UDS;
#else
        /* UDS关闭时忽略标准帧，避免误触发诊断服务。 */
        return APP_CAN_ID_NONE;
#endif
    }

    if (ptCanPacket->u32idType != D_CAN_ID_EXTENDED)
    {
        return APP_CAN_ID_NONE;
    }

    /* 28 83 03静默普通扩展帧；标准帧UDS仍继续进入诊断栈。 */
    if (!uds_normal_receive_enabled())
    {
        return APP_CAN_ID_NONE;
    }

    if (ducar_can_iap_protocol_is_packet(ptCanPacket))
    {
        return APP_CAN_ID_DUCAR_IAP;
    }

    /* 并联编址使用固定ID 0x18F10815，不交给普通DUCAR协议解析。 */
    if ((app_parallel_id_is_enabled() != 0U) &&
        (ptCanPacket->u32id == ID_ASSIGN_MSG))
    {
        return APP_CAN_ID_ASSIGN;
    }

    return APP_CAN_ID_DUCAR_NORMAL;
}

bool app_can_init(void)
{
    /* 先初始化底层CAN，再初始化普通协议和IAP协议的运行状态。 */
    if (!d_can_init(&can0))
    {
        SEGGER_RTT_WriteString(0, "CAN init failed\r\n");
        return false;
    }

    ducar_can_protocol_init();
    zs_b1_can_protocol_init();
    s_u8SilentWakePending = 0U;
    ducar_can_iap_protocol_init();
    app_bms_obc_init();
    /*
     * CAN协议初始化阶段清空并联包状态缓存；之后接收任务写对端状态，
     * 发送调度任务周期刷新本机状态。宏默认关闭时两条路径均早退。
     */
    app_parallel_status_init();
#if (APP_PARAM_UDS_ENABLE == 1U)
    service_28_reset();
    uds_init();
#endif
    return true;
}

void CAN_ReceiveTask(void)
{
    t_can_packet tCanPacket = {0};
    tAppCanIdType eCanIdType;
    uint8_t u8SilentWake;
    uint8_t u8ProtocolSilent;

    /*
     * 一次任务尽量把中断已经放入环形队列的报文全部取完。
     * 取出后按“标准帧/扩展帧 + PF/PGN”进入对应协议，不在这里直接组包。
     */
    while (d_can_read(&tCanPacket))
    {
        eCanIdType = app_can_id_check(&tCanPacket);
        /* 真正静默时任何CAN帧都是总线活动：先退出静默并恢复宗申发送会话。 */
        u8ProtocolSilent = zs_b1_can_protocol_is_silent() ? 1U : 0U;
        u8SilentWake = ((u8ProtocolSilent != 0U) ||
                        (s_u8SilentWakePending != 0U)) ? 1U : 0U;
        if (u8ProtocolSilent != 0U)
        {
            /* 先冻结并联缓存时钟，再让宗申协议退出真正静默，避免首帧恢复错序。 */
            app_parallel_status_resume_after_silence();
            app_parallel_id_resume_after_silence();
            zs_b1_can_protocol_notify_wakeup();
        }
        if (u8SilentWake != 0U)
        {
            /* 当前报文承接了静默唤醒事件；后续帧按正常活动报文处理。 */
            s_u8SilentWakePending = 0U;
        }

        /* 升级期间只分发IAP帧，防止普通请求在静默结束后形成待发报文。 */
        if (ducar_can_iap_protocol_is_busy() &&
            (eCanIdType != APP_CAN_ID_DUCAR_IAP) &&
            (eCanIdType != APP_CAN_ID_ECM_STATUS6))
        {
            continue;
        }

        /*
         * 在常规协议分类分发前尝试更新内部并联状态表，因此无需PC上位机打开普通报文显示。
         * 但IAP忙时已在上方过滤普通帧，这里也不会把升级期间帧计入并联在线状态。
         */
        /* 必须先于状态缓存对本机源地址帧的过滤，才能发现重复地址。 */
        app_parallel_id_note_can_frame(&tCanPacket);
        app_parallel_status_receive(&tCanPacket);

        if (eCanIdType == APP_CAN_ID_OBC)
        {
            app_bms_obc_receive(tCanPacket.au8data, tCanPacket.u8len);
        }
        else if (eCanIdType == APP_CAN_ID_ECM_STATUS6)
        {
            if (tCanPacket.u8len == APP_CAN_ECM_STATUS6_RX_DLC)
            {
                app_input_signals_note_can_key_frame();
            }
        }
        else if (eCanIdType == APP_CAN_ID_DUCAR_NORMAL)
        {
            ducar_can_protocol_receive(&tCanPacket);
        }
        else if (eCanIdType == APP_CAN_ID_ASSIGN)
        {
            if (app_can_id_assign_requested() &&
                !app_can_id_assign_paused())
            {
                if (u8SilentWake != 0U)
                {
                    /* 静默期UID帧立即作废旧rank，并以该帧启动新编址收集。 */
                    app_parallel_id_set_assignment_valid(0U);
                    app_id_assign_restart_with_frame(&tCanPacket);
                }
                else
                {
                    app_id_assign_msg_handle(&tCanPacket);
                }
            }
        }
        else if (eCanIdType == APP_CAN_ID_DUCAR_IAP)
        {
            ducar_can_iap_protocol_receive(&tCanPacket);
            if (ducar_can_iap_protocol_is_busy())
            {
                /* 收到升级启动帧即静默普通CAN，不等待后续发送调度周期。 */
                ducar_can_protocol_set_iap_silence(true);
            }
        }
        else if (eCanIdType == APP_CAN_ID_UDS)
        {
#if (APP_PARAM_UDS_ENABLE == 1U)
            /* 升级请求优先于校准会话，仅将已分类的标准帧交给UDS传输层。 */
            uds_recv_frame(tCanPacket.u32id, tCanPacket.au8data, tCanPacket.u8len);
#endif
        }
        else
        {
            /* 未知帧不进入任何协议，避免误触发普通协议或升级流程。 */
        }
    }
}

void CAN_TransmitTask(void)
{
    d_can_tx_diag_t tCanTxDiag;
    ducar_can_tx_queue_diag_t tProtocolTxDiag;
    uint8_t u8DetdTxWindowActive;
    uint8_t u8IapBusy;
    uint8_t u8IdAssignmentReady;

    d_can_service_tx();
    /*
     * 发送调度顺序：先维护CAN协议时基和快照，再处理控制回复、IAP回复，
     * 最后在DETD主动发送窗口内发送普通CAN周期报文。
     */
    ducar_can_bus_timer_handle();
    ducar_can_data_renew();
    /* 先更新Sleep窗口状态，再运行编址和在线缓存任务，确保窗口起点同周期生效。 */
    zs_b1_can_protocol_task(ducar_can_transport_is_active());
    u8IdAssignmentReady = app_can_id_assign_update() ? 1U : 0U;
    /* 定期刷新本机快照，并清理超时离线的对端状态。 */
    app_parallel_status_task();
    ducar_can_iap_protocol_task();
    u8IapBusy = ducar_can_iap_protocol_is_busy() ? 1U : 0U;
    ducar_can_protocol_set_iap_silence(u8IapBusy != 0U);
    if ((u8IapBusy == 0U) &&
        (u8IdAssignmentReady != 0U) &&
        !zs_b1_can_protocol_is_silent())
    {
        ducar_can_control_reply_task();
    }
    /* 离线时仍消耗DETD窗口，避免链路恢复后补发已过期的周期状态。 */
    u8DetdTxWindowActive = ducar_can_detd_tx_window_is_active() ? 1U : 0U;
    if ((u8IapBusy == 0U) &&
        (u8IdAssignmentReady != 0U) &&
        (u8DetdTxWindowActive != 0U) &&
        (app_parallel_id_is_address_conflict() == 0U) &&
        d_can_is_bus_online(&can0))
    {
		ducar_can_50ms_task();
        ducar_can_100ms_task();
        ducar_can_200ms_task();
        ducar_can_500ms_task();
        ducar_can_1000ms_task();
    }

    if (++s_u16AppCanTxDiagTicks >= 1000U)
    {
        s_u16AppCanTxDiagTicks = 0U;
        d_can_get_tx_diagnostics(&tCanTxDiag);
        ducar_can_get_tx_queue_diagnostics(&tProtocolTxDiag);
    }
}

bool app_can_is_sleep_allowed(void)
{
    if ((app_parallel_id_is_enabled() != 0U) &&
        app_can_id_assign_requested() &&
        (id_assign_state != ID_ASSIGNED))
    {
        return false;
    }

    return !ducar_can_transport_is_active() &&
           zs_b1_can_protocol_allows_sleep() &&
           ducar_can_cal2_allows_sleep() &&
           !ducar_can_iap_protocol_is_busy();
}

bool app_can_is_zs_idle_timeout_complete(void)
{
    return zs_b1_can_protocol_idle_timeout_complete();
}

void app_can_notify_wakeup(void)
{
    if (zs_b1_can_protocol_is_silent())
    {
        /* CAN在STOP期间关闭，保留唤醒前的静默状态供恢复后的首帧识别。 */
        s_u8SilentWakePending = 1U;
        app_parallel_status_resume_after_silence();
        app_parallel_id_resume_after_silence();
    }
    zs_b1_can_protocol_notify_wakeup();
}

bool app_can_iap_is_busy(void)
{
    return ducar_can_iap_protocol_is_busy();
}
