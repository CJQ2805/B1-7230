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
#include "App/app_current_calib.h"
#include "App/app_bms_obc.h"
#include "App/app_input_signals.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "Protocol/Comm/zs_b1_can_protocol.h"
#include "Protocol/IAP/ducar_can_iap_protocol.h"
#include "Protocol/UDS/UDSBase/uds_port.h"
#include "Protocol/UDS/UDSLogic/SID28_CommunicationControl.h"
#include "SEGGER_RTT.h"

typedef enum
{
    APP_CAN_ID_NONE = 0U,
    APP_CAN_ID_OBC,
    APP_CAN_ID_ECM_STATUS6,
    APP_CAN_ID_UDS,
    APP_CAN_ID_DUCAR_NORMAL,
    APP_CAN_ID_DUCAR_IAP
} app_can_id_type_t;

#define APP_CAN_ECM_STATUS6_RX_CAN_ID  0x105U
#define APP_CAN_ECM_STATUS6_RX_DLC     8U

/* Comm_Task约1ms调用一次CAN任务，诊断每1000次汇总输出，避免逐帧刷屏。 */
static uint16_t s_app_can_tx_diag_ticks;

/*
 * 根据帧格式和CAN ID判断协议归属。
 *
 * 标准帧预留给UDS；扩展帧先判断DUCAR CAN IAP，再交给普通DUCAR协议。
 * 这里不判断业务数据是否正确，业务合法性由具体协议模块继续判断。
 */
static app_can_id_type_t app_can_id_check(const t_can_packet *ptCanPacket)
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
    ducar_can_iap_protocol_init();
    app_bms_obc_init();
#if (APP_PARAM_UDS_ENABLE == 1U)
    service_28_reset();
    uds_init();
#endif
    return true;
}

void CAN_ReceiveTask(void)
{
    t_can_packet tCanPacket = {0};
    app_can_id_type_t eCanIdType;

    /*
     * 一次任务尽量把中断已经放入环形队列的报文全部取完。
     * 取出后按“标准帧/扩展帧 + PF/PGN”进入对应协议，不在这里直接组包。
     */
    while (d_can_read(&tCanPacket))
    {
        eCanIdType = app_can_id_check(&tCanPacket);
        /* 升级期间只分发IAP帧，防止普通请求在静默结束后形成待发报文。 */
        if (ducar_can_iap_protocol_is_busy() &&
            (eCanIdType != APP_CAN_ID_DUCAR_IAP) &&
            (eCanIdType != APP_CAN_ID_ECM_STATUS6))
        {
            continue;
        }

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
        else if (eCanIdType == APP_CAN_ID_DUCAR_IAP)
        {
            if (!app_current_calib_cal2_is_active())
            {
                ducar_can_iap_protocol_receive(&tCanPacket);
                if (ducar_can_iap_protocol_is_busy())
                {
                    /* 收到升级启动帧即静默普通CAN，不等待后续发送调度周期。 */
                    ducar_can_protocol_set_iap_silence(true);
                }
            }
        }
        else if (eCanIdType == APP_CAN_ID_UDS)
        {
#if (APP_PARAM_UDS_ENABLE == 1U)
            /* 仅将已分类的标准帧交给UDS传输层。 */
            if (!app_current_calib_cal2_is_active())
            {
                uds_recv_frame(tCanPacket.u32id, tCanPacket.au8data, tCanPacket.u8len);
            }
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

    d_can_service_tx();
    /*
     * 发送调度顺序：先维护CAN协议时基和快照，再处理控制回复、IAP回复，
     * 最后在DETD主动发送窗口内发送普通CAN周期报文。
     */
    ducar_can_bus_timer_handle();
    ducar_can_data_renew();
    /* 宗申B1主动发送受KEY状态门控；KEY缺失时保留电流及DETD/DETC原触发逻辑。 */
    zs_b1_can_protocol_task();
    ducar_can_iap_protocol_task();
    u8IapBusy = ducar_can_iap_protocol_is_busy() ? 1U : 0U;
    ducar_can_protocol_set_iap_silence(u8IapBusy != 0U);
    if (u8IapBusy == 0U)
    {
        ducar_can_control_reply_task();
    }
    /* 离线时仍消耗DETD窗口，避免链路恢复后补发已过期的周期状态。 */
    u8DetdTxWindowActive = ducar_can_detd_tx_window_is_active() ? 1U : 0U;
    if ((u8IapBusy == 0U) &&
        (u8DetdTxWindowActive != 0U) &&
        d_can_is_bus_online(&can0))
    {
		ducar_can_50ms_task();
        ducar_can_100ms_task();
        ducar_can_200ms_task();
        ducar_can_500ms_task();
        ducar_can_1000ms_task();
    }

    if (++s_app_can_tx_diag_ticks >= 1000U)
    {
        s_app_can_tx_diag_ticks = 0U;
        d_can_get_tx_diagnostics(&tCanTxDiag);
        ducar_can_get_tx_queue_diagnostics(&tProtocolTxDiag);
    }
}

bool app_can_is_sleep_allowed(void)
{
    return zs_b1_can_protocol_allows_sleep() &&
           ducar_can_cal2_allows_sleep() &&
           !ducar_can_iap_protocol_is_busy();
}

bool app_can_is_zs_idle_timeout_complete(void)
{
    return zs_b1_can_protocol_idle_timeout_complete();
}

void app_can_notify_wakeup(void)
{
    zs_b1_can_protocol_notify_wakeup();
}

bool app_can_iap_is_busy(void)
{
    return ducar_can_iap_protocol_is_busy();
}
