/*
 * 文件作用：
 *   根据 BMS 快照和外部控制请求，判断充电 MOS、放电 MOS、预放 MOS 是否允许打开。
 *
 * 正常运行时，每个10ms周期的判断顺序：
 *   1. 判断充电 MOS；
 *   2. 判断放电路径是否允许；
 *   3. 处理预放电；
 *   4. 判断放电 MOS；
 *   5. 通过 Board 端口下发输出。
 *
 * 术语：闭合/打开/ON/1表示允许MOS导通；断开/关闭/OFF/0表示不导通。
 * CMOS为充电MOS，DMOS为放电MOS，PDMOS为预放MOS；PDSG表示预放电流程。
 * 电流符号来自校准及死区处理后的业务电流：正值为充电，负值为放电。
 * tcmd是本轮请求下发的命令，tstat中的CMOS/DMOS状态来自AFE反馈；
 * PDMOS状态是软件输出命令的镜像，并非独立硬件反馈。
 * 单包与并联由task入口分流；唤醒等待期间使用各自的保持输出分支。
 * 本文件只包含 MOS 业务规则，不包含具体 AFE 或 GPIO 操作。
 * PDSG使用注入的毫秒时基，远程命令有效期使用RTC日内秒数。
 */

#include "app_h_power_manage.h"

#include "App/app_bms_fault.h"
#include "App/app_bms_snapshot.h"
#include "App/app_parallel_id.h"
#include "Board/board_power_port.h"
#include "Driver/d_rtc.h"

#define APP_H_POWER_PDSG_DELAY_DEFAULT_MS 300U
#define APP_H_POWER_REMOTE_MOS_COUNT 3U
#define APP_H_POWER_REMOTE_MOS_DEFAULT_DURATION_MIN 1U
#define APP_H_POWER_REMOTE_MOS_SECONDS_PER_MINUTE 60UL
#define APP_H_POWER_REMOTE_MOS_SECONDS_PER_DAY 86400UL

#define APP_H_POWER_REMOTE_PDSG_INDEX  0U
#define APP_H_POWER_REMOTE_CMOS_INDEX  1U
#define APP_H_POWER_REMOTE_DMOS_INDEX  2U

/* 预放状态：等待启动、PDMOS计时中、允许DMOS并等待其闭合反馈。 */
#define APP_H_POWER_PDSG_STEP_IDLE  0U
#define APP_H_POWER_PDSG_STEP_DELAY 2U
#define APP_H_POWER_PDSG_STEP_DMOS  3U

/* 全局模块对象：保留现有工程的观察和调用方式。 */
app_h_power_manage_t gt_app_h_power_manage;

/* WB4 业务层的直接初始化模式：端口和固定配置由管理器内部拥有。 */
static const app_h_power_manage_config_t s_app_h_power_config =
{
    0U
};
static app_h_power_manage_port_t s_app_h_power_port;

/* 将远程MOS槽位序号映射到对应的强制命令字段。 */
static int8_t *app_h_power_manage_remote_force_slot(app_h_power_manage_request_t *request,
                                                     uint8_t u8Index)
{
    if (request == 0)
    {
        return 0;
    }

    switch (u8Index)
    {
        case APP_H_POWER_REMOTE_PDSG_INDEX:
            return &request->i8pdmos_ctl_force[MF_Remote];

        case APP_H_POWER_REMOTE_CMOS_INDEX:
            return &request->i8cmos_ctl_force[MF_Remote];

        case APP_H_POWER_REMOTE_DMOS_INDEX:
            return &request->i8dmos_ctl_force[MF_Remote];

        default:
            return 0;
    }
}

/* 读取RTC日内秒数作为相对计时，不要求用户先校准日历。 */
static uint8_t app_h_power_manage_read_rtc_seconds(uint32_t *seconds)
{
    return (d_rtc_read_seconds_of_day(seconds) == true) ? 1U : 0U;
}

/* 判断任一路远程MOS是否仍持有有效期。 */
static uint8_t app_h_power_manage_remote_timer_active(
    const app_h_power_manage_t *module)
{
    uint8_t u8Index;

    for (u8Index = 0U; u8Index < APP_H_POWER_REMOTE_MOS_COUNT; u8Index++)
    {
        if (module->remote_mos_timer_active[u8Index] != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

/* RTC日内计时允许跨午夜；远程命令最长255分钟，小于一天。 */
static uint8_t app_h_power_manage_remote_command_expired(
    const app_h_power_manage_t *module,
    uint8_t u8Index,
    uint32_t u32NowSeconds)
{
    uint32_t u32StartSeconds = module->remote_mos_start_seconds[u8Index];
    uint32_t u32ElapsedSeconds;

    if ((u32StartSeconds >= APP_H_POWER_REMOTE_MOS_SECONDS_PER_DAY) ||
        (module->remote_mos_duration_seconds[u8Index] == 0UL))
    {
        return 1U;
    }

    u32ElapsedSeconds = (u32NowSeconds >= u32StartSeconds) ?
        (u32NowSeconds - u32StartSeconds) :
        (APP_H_POWER_REMOTE_MOS_SECONDS_PER_DAY - u32StartSeconds + u32NowSeconds);

    return (u32ElapsedSeconds >= module->remote_mos_duration_seconds[u8Index]) ? 1U : 0U;
}

/* RTC不可用或时钟回拨时失效关闭；正常情况下到期后释放远程强制源。 */
static void app_h_power_manage_expire_remote_commands(
    app_h_power_manage_t *module,
    app_h_power_manage_request_t *request)
{
    uint32_t u32NowSeconds = 0UL;
    uint8_t u8RtcValid;
    uint8_t u8Index;

    if ((module == 0) || (request == 0) ||
        (app_h_power_manage_remote_timer_active(module) == 0U))
    {
        return;
    }

    u8RtcValid = app_h_power_manage_read_rtc_seconds(&u32NowSeconds);
    for (u8Index = 0U; u8Index < APP_H_POWER_REMOTE_MOS_COUNT; u8Index++)
    {
        int8_t *pi8RequestForce;
        uint8_t u8Expired;

        if (module->remote_mos_timer_active[u8Index] == 0U)
        {
            continue;
        }

        u8Expired = ((u8RtcValid == 0U) ||
                     (app_h_power_manage_remote_command_expired(module,
                                                                u8Index,
                                                                u32NowSeconds) != 0U)) ? 1U : 0U;

        if (u8Expired != 0U)
        {
            pi8RequestForce = app_h_power_manage_remote_force_slot(request, u8Index);
            if (pi8RequestForce != 0)
            {
                *pi8RequestForce = APP_MOS_IGNORE;
            }
            pi8RequestForce = app_h_power_manage_remote_force_slot(&module->tctl, u8Index);
            if (pi8RequestForce != 0)
            {
                *pi8RequestForce = APP_MOS_IGNORE;
            }
            module->remote_mos_start_seconds[u8Index] = 0UL;
            module->remote_mos_duration_seconds[u8Index] = 0UL;
            module->remote_mos_timer_active[u8Index] = 0U;
        }
    }
}

/* 按MF编号从小到大找首个非IGNORE命令；这里只比较强制来源，不代表高于电流优先分支。 */
static int8_t app_h_power_manage_judge_force(const int8_t *force_list, uint8_t count)
{
    uint8_t u8ForceIndex;

    for (u8ForceIndex = 0U; u8ForceIndex < count; u8ForceIndex++)
    {
        if (force_list[u8ForceIndex] != APP_MOS_IGNORE)
        {
            return force_list[u8ForceIndex];
        }
    }

    return APP_MOS_IGNORE;
}

/* 检查 MOS 和时基端口是否全部已经注入。 */
static uint8_t app_h_power_manage_port_valid(const app_h_power_manage_port_t *port)
{
    if (port == 0)
    {
        return 0U;
    }

    if ((port->set_charge_mos == 0) ||
        (port->set_discharge_mos == 0) ||
        (port->set_precharge_mos == 0) ||
        (port->get_time_ms == 0))
    {
        return 0U;
    }

    return 1U;
}

/* 清空运行状态，并设置上电后的默认控制条件。 */
static void app_h_power_manage_reset(app_h_power_manage_t *module)
{
    uint8_t u8ForceIndex;
    uint8_t u8RemoteIndex;

    module->tctl.u8pdsg_enable = 0U;
    module->tctl.i8cmos_ctl_det = APP_MOS_ON;
    module->tctl.i8cmos_ctl_auth = APP_MOS_ON;
    module->tctl.i8dmos_ctl_det = APP_MOS_ON;
    module->tctl.i8dmos_ctl_auth = APP_MOS_ON;
    module->tctl.u8cmos_fault_forbid = 0U;
    module->tctl.u8dmos_fault_forbid = 0U;
    module->tctl.u8dmos_reverse_current_forbid = 0U;

    for (u8ForceIndex = 0U; u8ForceIndex < MF_Max; u8ForceIndex++)
    {
        module->tctl.i8cmos_ctl_force[u8ForceIndex] = APP_MOS_IGNORE;
        module->tctl.i8dmos_ctl_force[u8ForceIndex] = APP_MOS_IGNORE;
        module->tctl.i8pdmos_ctl_force[u8ForceIndex] = APP_MOS_IGNORE;
    }

    module->tcmd.u8cmos_ctl = 0U;
    module->tcmd.u8dmos_ctl = 0U;
    module->tcmd.u8pdmos_ctl = 0U;
    module->tcmd.u8efuse_ctl = 0U;

    module->tstat.u8chg_status = 0U;
    module->tstat.u8dsg_path_allowed = 0U;
    module->tstat.u8chgmos_state = 0U;
    module->tstat.u8dsgmos_state = 0U;
    module->tstat.u8pdsgmos_state = 0U;

    module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_IDLE;
    module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_OFF;
    module->pdsg_start_tick_ms = 0U;
    module->wakeup_current_update_seq = 0UL;
    module->wakeup_fresh_sample_required = 0U;
    module->wakeup_fresh_sample_seen = 0U;
    for (u8RemoteIndex = 0U; u8RemoteIndex < APP_H_POWER_REMOTE_MOS_COUNT; u8RemoteIndex++)
    {
        module->remote_mos_start_seconds[u8RemoteIndex] = 0UL;
        module->remote_mos_duration_seconds[u8RemoteIndex] = 0UL;
        module->remote_mos_timer_active[u8RemoteIndex] = 0U;
    }

    /* 第一次下发必须执行，即使安全初值也是关闭。 */
    module->last_cmos_cmd = APP_MOS_IGNORE;
    module->last_dmos_cmd = APP_MOS_IGNORE;
    module->last_pdmos_cmd = APP_MOS_IGNORE;
}

/*
 * 判断并生成充电 MOS 输出。
 *
 * 放电电流具有最高优先级，闭合CMOS以避免电流通过体二极管。
 * 无放电电流时，再判断强制命令、故障、唤醒采样、数据和授权。
 * 普通运行仅要求电流序号非0（曾成功采样），这里没有检查500ms新鲜度。
 */
static void app_h_power_manage_cmos_ctrl(app_h_power_manage_t *module,
                                         const app_bms_snapshot_t *snapshot,
                                         const app_h_power_manage_request_t *request)
{
    int8_t i8ForcedCommand;

    module->tcmd.u8cmos_ctl = 0U;
    module->tstat.u8chg_status = 0U;

    if ((snapshot->discharging != 0U) &&
        (snapshot->afe_current_update_seq != 0UL))
    {
        /* 放电电流优先于CMOS强制关闭及充电、通用、PF故障。 */
        module->tcmd.u8cmos_ctl = 1U;
        module->tstat.u8chg_status = 1U;
        return;
    }

    i8ForcedCommand = app_h_power_manage_judge_force(module->tctl.i8cmos_ctl_force, MF_Max);
    if (i8ForcedCommand == APP_MOS_OFF)
    {
        /* 没有放电电流优先条件时，最高优先级的强制关断生效。 */
        return;
    }

    if ((request->u8cmos_fault_forbid != 0U) &&
        (i8ForcedCommand != APP_MOS_ON))
    {
        /* 最高优先级强制命令不是开启时，充电保护、通用故障或PF故障禁止闭合CMOS。 */
        return;
    }

    if ((module->wakeup_fresh_sample_required != 0U) &&
        (i8ForcedCommand != APP_MOS_OFF))
    {
        /* 唤醒后先等AFE新采样，强制开启不能绕过新样本门控。 */
        return;
    }

    if (snapshot->data_ready == 0U)
    {
        /* 电芯数据没有准备好，禁止闭合充电 MOS。 */
        return;
    }

    if ((module->tctl.i8cmos_ctl_det == APP_MOS_OFF) ||
        (module->tctl.i8cmos_ctl_auth == APP_MOS_OFF))
    {
        /* 检测或授权软件许可为OFF时禁止充电；这里的检测字段不是DETD引脚。 */
        return;
    }

    if (i8ForcedCommand == APP_MOS_ON)
    {
        /* 最高业务优先级远程开启请求；数据、新样本、检测和授权门控已通过。 */
        module->tcmd.u8cmos_ctl = 1U;
        module->tstat.u8chg_status = 1U;
        return;
    }

    /* 充电数据有效、无故障、所有外部条件允许，打开 CMOS。 */
    module->tcmd.u8cmos_ctl = 1U;
    module->tstat.u8chg_status = 1U;
}

/*
 * 判断放电路径是否允许。
 *
 * 这个函数只负责形成放电路径资格，不直接控制 DMOS；最高优先级强制开启请求可覆盖
 * 软件故障禁止，但不能绕过数据有效、唤醒采样和 DETD 在位条件。
 * 这样查放电 MOS 时，可以先看 tstat.u8dsg_path_allowed，再看 DMOS 输出逻辑。
 * 此资格主要控制正常放电/预放，不等同DMOS实际反馈；正向电流旁路可在资格为0时闭合DMOS。
 */
static void app_h_power_manage_dsg_status_judge(app_h_power_manage_t *module,
                                                const app_bms_snapshot_t *snapshot,
                                                const app_h_power_manage_request_t *request)
{
    int8_t i8DmosForcedCommand;
    int8_t i8PdmosForcedCommand;

    module->tstat.u8dsg_path_allowed = 0U;

    if (module->wakeup_fresh_sample_required != 0U)
    {
        /* AFE新采样尚未完成前，不允许重新启动放电或预放流程。 */
        return;
    }

    if (snapshot->data_ready == 0U)
    {
        /* 电芯数据没有准备好，禁止放电。 */
        return;
    }

    if (snapshot->signal.input.mcu_detd == 0U)
    {
        /* 稳定DETD移除时撤销正常放电及预放资格；正向电流旁路另在DMOS函数判断。 */
        return;
    }

    i8DmosForcedCommand =
        app_h_power_manage_judge_force(request->i8dmos_ctl_force, MF_Max);
    i8PdmosForcedCommand =
        app_h_power_manage_judge_force(request->i8pdmos_ctl_force, MF_Max);
    if ((request->u8dmos_fault_forbid != 0U) &&
        (i8DmosForcedCommand != APP_MOS_ON) &&
        (i8PdmosForcedCommand != APP_MOS_ON))
    {
        /* DMOS和PDMOS均无最高优先级强制开启时，放电保护、通用故障或PF故障禁止放电。 */
        return;
    }

    if (i8DmosForcedCommand == APP_MOS_OFF)
    {
        /* DMOS最高优先级强制命令为关闭时，一并中止当前预放资格。 */
        return;
    }

    if ((module->tctl.i8dmos_ctl_det == APP_MOS_OFF) ||
        (module->tctl.i8dmos_ctl_auth == APP_MOS_OFF))
    {
        /* 检测或授权软件许可为OFF时禁止正常放电；i8dmos_ctl_det不是DETD引脚。 */
        return;
    }

    /* 放电数据有效、无故障、所有外部条件允许。 */
    module->tstat.u8dsg_path_allowed = 1U;
}

/*
 * 处理预放电状态机：
 *   IDLE  -> 资格允许后闭合PDMOS，并记录开始时间；
 *   DELAY -> PDMOS保持闭合，计时达到配置值（默认300ms）后允许DMOS；
 *   DMOS  -> 允许DMOS，AFE反馈DMOS闭合后断开PDMOS。
 * 正向电流旁路会在DMOS判断中覆盖本轮PDMOS命令；具体保持/复位见DMOS函数。
 * 预放只按时间和DMOS反馈推进，这里没有检查负载电压，也没有等待反馈的超时。
 */
static void app_h_power_manage_reset_pdsg(app_h_power_manage_t *module)
{
    module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_IDLE;
    module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_OFF;
    module->pdsg_start_tick_ms = 0U;
}

static void app_h_power_manage_pdsg_ctrl(app_h_power_manage_t *module)
{
    int8_t i8ForcedCommand;
    uint32_t u32CurrentTimeMs;

    module->tcmd.u8pdmos_ctl = 0U;

    if ((module->tctl.u8pdsg_enable == 0U) ||
        (module->tstat.u8dsg_path_allowed == 0U))
    {
        /* 预放使能或正常放电资格被撤销：断开PDMOS，清除计时和DMOS预放许可。 */
        app_h_power_manage_reset_pdsg(module);
        return;
    }

    i8ForcedCommand = app_h_power_manage_judge_force(module->tctl.i8pdmos_ctl_force, MF_Max);
    if (i8ForcedCommand != APP_MOS_IGNORE)
    {
        /* 路径允许后，PDMOS强制命令直接决定本轮PDMOS；强制闭合不会推进预放计时。 */
        /* 预放被强制关闭且 DMOS 尚未闭合时，不能让旧计时继续积累资格。 */
        if ((i8ForcedCommand == APP_MOS_OFF) &&
            (module->tstat.u8dsgmos_state == 0U))
        {
            app_h_power_manage_reset_pdsg(module);
        }
        module->tcmd.u8pdmos_ctl = (uint8_t)(i8ForcedCommand == APP_MOS_ON);
        return;
    }

    switch (module->tpdsg.u8step)
    {
        case APP_H_POWER_PDSG_STEP_IDLE:
            /* 放电重新允许后，从此刻开始一轮完整预放。 */
            module->tcmd.u8pdmos_ctl = 1U;
            module->pdsg_start_tick_ms = module->port.get_time_ms();
            module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_DELAY;
            break;

        case APP_H_POWER_PDSG_STEP_DELAY:
            /* 预放时间未到之前，PDMOS 保持打开。 */
            module->tcmd.u8pdmos_ctl = 1U;
            u32CurrentTimeMs = module->port.get_time_ms();
            if ((u32CurrentTimeMs - module->pdsg_start_tick_ms) >= module->config.pdsg_delay_ms)
            {
                /* 预放完成，下一步允许 DMOS。 */
                module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_ON;
                module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_DMOS;
            }
            break;

        case APP_H_POWER_PDSG_STEP_DMOS:
            /* DMOS 尚未反馈闭合时保持 PDMOS，闭合后释放 PDMOS。 */
            module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_ON;
            module->tcmd.u8pdmos_ctl =
                (module->tstat.u8dsgmos_state == 0U) ? 1U : 0U;
            break;

        default:
            /* 异常状态回到安全状态。 */
            module->tcmd.u8pdmos_ctl = 0U;
            app_h_power_manage_reset_pdsg(module);
            break;
    }
}

/*
 * 判断并生成放电 MOS 输出。
 *
 * 强制关断优先；正向电流可覆盖放电保护，但无强制开启时不能覆盖通用/PF故障。
 * 没有正向电流时，DMOS受放电路径和预放状态门控。
 * 正向电流旁路可以越过DETD、数据就绪和预放资格；普通运行只检查采样序号非0。
 */
static void app_h_power_manage_dmos_ctrl(app_h_power_manage_t *module,
                                         const app_bms_snapshot_t *snapshot,
                                         const app_h_power_manage_request_t *request)
{
    int8_t i8ForcedCommand;

    module->tcmd.u8dmos_ctl = 0U;

    i8ForcedCommand = app_h_power_manage_judge_force(module->tctl.i8dmos_ctl_force, MF_Max);
    if (i8ForcedCommand == APP_MOS_OFF)
    {
        /* 关闭命令立即生效，并由放电路径判断复位预放资格。 */
        return;
    }

    if ((snapshot->charging != 0U) &&
        (snapshot->afe_current_update_seq != 0UL) &&
        ((request->u8dmos_reverse_current_forbid == 0U) ||
         (i8ForcedCommand == APP_MOS_ON)))
    {
        /* 正向电流可覆盖放电保护；无强制开启时，通用/PF故障仍禁止。 */
        module->tcmd.u8dmos_ctl = 1U;
        module->tcmd.u8pdmos_ctl = 0U;
        if ((module->tstat.u8dsg_path_allowed == 0U) ||
            (module->tpdsg.u8step != APP_H_POWER_PDSG_STEP_DMOS))
        {
            /* 路径不允许或预放尚未完成时，PDMOS已被旁路关断，不保留未实际执行的计时。 */
            app_h_power_manage_reset_pdsg(module);
        }
        return;
    }

    if ((request->u8dmos_fault_forbid != 0U) &&
        (i8ForcedCommand != APP_MOS_ON))
    {
        /* DMOS最高优先级强制命令不是开启时，放电保护、通用故障或PF故障禁止闭合。 */
        return;
    }

    if (module->tstat.u8dsg_path_allowed == 0U)
    {
        /* 放电条件不满足，关闭 DMOS。 */
        return;
    }

    if (module->tpdsg.i8dmos_ctl_pdsg != APP_MOS_ON)
    {
        /* 强制打开也不能跳过 DETD 接入后的预放流程。 */
        return;
    }

    /* 放电允许且预放完成，打开 DMOS。 */
    module->tcmd.u8dmos_ctl = 1U;
}

/*
 * 稳定DETD信号决定是否允许预放；每次放电路径关闭后，预放状态机自行复位。
 */
static void app_h_power_manage_update_pdsg_request(
    const app_bms_snapshot_t *snapshot,
    app_h_power_manage_request_t *request)
{
    if ((snapshot == 0) || (request == 0))
    {
        return;
    }

    request->u8pdsg_enable = (snapshot->signal.input.mcu_detd != 0U) ? 1U : 0U;
}

/* 只有输出命令变化时才调用硬件端口；端口返回成功后才更新上次下发命令缓存。 */
static app_result_t app_h_power_manage_apply_mos(app_h_power_manage_t *module)
{
    app_result_t eApplyResult;
    int8_t i8ChargeMosCommand;
    int8_t i8DischargeMosCommand;
    int8_t i8PrechargeMosCommand;

    if ((module == 0) || (app_h_power_manage_port_valid(&module->port) == 0U))
    {
        return APP_RESULT_NOT_INIT;
    }

    i8ChargeMosCommand = (int8_t)module->tcmd.u8cmos_ctl;
    i8DischargeMosCommand = (int8_t)module->tcmd.u8dmos_ctl;
    i8PrechargeMosCommand = (int8_t)module->tcmd.u8pdmos_ctl;
    /* PDMOS没有独立反馈，此状态仅镜像软件命令，不能证明硬件实际闭合。 */
    module->tstat.u8pdsgmos_state = module->tcmd.u8pdmos_ctl;

    if ((i8ChargeMosCommand == module->last_cmos_cmd) &&
        (i8DischargeMosCommand == module->last_dmos_cmd) &&
        (i8PrechargeMosCommand == module->last_pdmos_cmd))
    {
        return APP_RESULT_OK;
    }

    if (module->port.set_all_mos != 0)
    {
        /* 当前 Board 端口通过 app_hw_apply_control() 一次性落地三路 MOS。 */
        eApplyResult = module->port.set_all_mos(module->tcmd.u8cmos_ctl,
                                                 module->tcmd.u8dmos_ctl,
                                                 module->tcmd.u8pdmos_ctl);
    }
    else
    {
        /* 保留逐路回调作为兼容后备，旧板级端口仍可继续使用。 */
        eApplyResult = module->port.set_charge_mos(module->tcmd.u8cmos_ctl);
        if (eApplyResult == APP_RESULT_OK)
        {
            eApplyResult = module->port.set_discharge_mos(module->tcmd.u8dmos_ctl);
        }
        if (eApplyResult == APP_RESULT_OK)
        {
            eApplyResult = module->port.set_precharge_mos(module->tcmd.u8pdmos_ctl);
        }
    }

    if (eApplyResult != APP_RESULT_OK)
    {
        return eApplyResult;
    }

    module->last_cmos_cmd = i8ChargeMosCommand;
    module->last_dmos_cmd = i8DischargeMosCommand;
    module->last_pdmos_cmd = i8PrechargeMosCommand;
    return APP_RESULT_OK;
}

/*
 * 单包唤醒等待：默认保持休眠前命令，不进行正常预放。
 * 唤醒后新负电流可优先闭合CMOS；新正电流可按DMOS旁路条件闭合DMOS。
 * 无上述旁路时，已出现的强制关断、故障或许可撤销等条件仍可断开对应MOS。
 * 新样本要求序号非0且不同于唤醒时记录的序号，不等同于500ms新鲜度判断。
 */
static app_result_t app_h_power_manage_hold_wakeup_outputs(
    app_h_power_manage_t *module,
    const app_bms_snapshot_t *snapshot,
    const app_h_power_manage_request_t *request,
    uint8_t dmos_feedback_dropped)
{
    int8_t i8CmosForcedCommand;
    int8_t i8DmosForcedCommand;
    int8_t i8PdmosForcedCommand;
    uint8_t u8DmosMustTurnOff;
    app_result_t eApplyResult;

    i8CmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8cmos_ctl_force, MF_Max);
    if ((snapshot->discharging != 0U) &&
        (snapshot->afe_current_update_seq != 0UL) &&
        (snapshot->afe_current_update_seq != module->wakeup_current_update_seq))
    {
        /* 唤醒后取得新放电电流样本时，同样优先闭合CMOS。 */
        module->tcmd.u8cmos_ctl = 1U;
    }
    else if ((i8CmosForcedCommand == APP_MOS_OFF) ||
        ((request->u8cmos_fault_forbid != 0U) && (i8CmosForcedCommand != APP_MOS_ON)) ||
        (module->tctl.i8cmos_ctl_det == APP_MOS_OFF) ||
        (module->tctl.i8cmos_ctl_auth == APP_MOS_OFF))
    {
        module->tcmd.u8cmos_ctl = 0U;
    }
    module->tstat.u8chg_status = module->tcmd.u8cmos_ctl;

    i8DmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8dmos_ctl_force, MF_Max);
    i8PdmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8pdmos_ctl_force, MF_Max);
    if ((i8DmosForcedCommand != APP_MOS_OFF) &&
        (snapshot->charging != 0U) &&
        (snapshot->afe_current_update_seq != 0UL) &&
        (snapshot->afe_current_update_seq != module->wakeup_current_update_seq) &&
        ((request->u8dmos_reverse_current_forbid == 0U) ||
         (i8DmosForcedCommand == APP_MOS_ON)))
    {
        /* 唤醒后的新正向电流样本也可覆盖放电保护，保留强制命令及通用/PF故障的原优先级。 */
        module->tcmd.u8dmos_ctl = 1U;
        module->tcmd.u8pdmos_ctl = 0U;
        u8DmosMustTurnOff = 0U;
    }
    else
    {
        /* 不满足正电流旁路时，只计算是否必须关断；否则沿用休眠前DMOS命令。 */
        u8DmosMustTurnOff = ((i8DmosForcedCommand == APP_MOS_OFF) ||
                         ((request->u8dmos_fault_forbid != 0U) &&
                          (i8DmosForcedCommand != APP_MOS_ON) &&
                          (i8PdmosForcedCommand != APP_MOS_ON)) ||
                         (module->tctl.i8dmos_ctl_det == APP_MOS_OFF) ||
                         (module->tctl.i8dmos_ctl_auth == APP_MOS_OFF) ||
                         (module->tctl.u8pdsg_enable == 0U) ||
                         (dmos_feedback_dropped != 0U)) ? 1U : 0U;
    }

    if (u8DmosMustTurnOff != 0U)
    {
        module->tcmd.u8dmos_ctl = 0U;
        module->tcmd.u8pdmos_ctl = 0U;
        module->tstat.u8dsg_path_allowed = 0U;
        app_h_power_manage_reset_pdsg(module);
    }
    else if (i8PdmosForcedCommand == APP_MOS_OFF)
    {
        module->tcmd.u8pdmos_ctl = 0U;
        if (module->tcmd.u8dmos_ctl == 0U)
        {
            app_h_power_manage_reset_pdsg(module);
        }
    }

    eApplyResult = app_h_power_manage_apply_mos(module);
    if (eApplyResult != APP_RESULT_OK)
    {
        return eApplyResult;
    }

    module->tcmd.u8efuse_ctl = app_bms_fault_get_efuse_cmd();
    return APP_RESULT_OK;
}

/* 低功耗入口保存当前输出；唤醒后保持硬件状态，并在新样本到达后恢复完整判定。 */
void app_h_power_manage_prepare_sleep(app_h_power_manage_output_t *output)
{
    if (output == 0)
    {
        return;
    }

    *output = gt_app_h_power_manage.tcmd;
}

static app_result_t app_h_power_manage_init_module(app_h_power_manage_t *module,
                                                    const app_h_power_manage_config_t *config,
                                                    const app_h_power_manage_port_t *port)
{
    app_result_t eInitResult;

    if (module == 0)
    {
        return APP_RESULT_NULL_PTR;
    }

    if (app_h_power_manage_port_valid(port) == 0U)
    {
        module->initialized = 0U;
        return APP_RESULT_NOT_INIT;
    }

    module->config.pdsg_delay_ms = APP_H_POWER_PDSG_DELAY_DEFAULT_MS;
    if ((config != 0) && (config->pdsg_delay_ms != 0U))
    {
        module->config.pdsg_delay_ms = config->pdsg_delay_ms;
    }
    module->port = *port;
    app_h_power_manage_reset(module);

    /* 初始化阶段先把三路 MOS 置于安全关闭状态；EFUSE由故障模块统一管理。 */
    eInitResult = app_h_power_manage_apply_mos(module);
    if (eInitResult != APP_RESULT_OK)
    {
        return eInitResult;
    }

    module->initialized = 1U;
    return APP_RESULT_OK;
}

/* 当前工程初始化入口：获取Board端口并初始化三路MOS；EFUSE由故障模块管理。 */
app_result_t app_h_power_manage_init(void)
{
    s_app_h_power_port = app_board_power_port_get();

    return app_h_power_manage_init_module(&gt_app_h_power_manage,
                                          &s_app_h_power_config,
                                          &s_app_h_power_port);
}

app_result_t app_h_power_manage_process(app_h_power_manage_t *module,
                                        const app_bms_snapshot_t *snapshot,
                                        const app_h_power_manage_request_t *request)
{
    app_result_t eProcessResult;
    app_h_power_manage_request_t tCurrentRequest;
    uint8_t u8DmosFeedbackDropped;

    if ((module == 0) || (snapshot == 0) || (request == 0))
    {
        return APP_RESULT_NULL_PTR;
    }

    if (module->initialized == 0U)
    {
        return APP_RESULT_NOT_INIT;
    }

    /* 保存本轮输入，便于仿真窗口直接查看模块收到的控制条件。 */
    tCurrentRequest = *request;
    app_h_power_manage_expire_remote_commands(module, &tCurrentRequest);
    app_h_power_manage_update_pdsg_request(snapshot, &tCurrentRequest);
    module->tctl = tCurrentRequest;

    if (module->wakeup_fresh_sample_required != 0U)
    {
        if ((snapshot->data_ready != 0U) &&
            (snapshot->afe_status_valid != 0U) &&
            (snapshot->afe_current_update_seq != module->wakeup_current_update_seq))
        {
            if (module->wakeup_fresh_sample_seen != 0U)
            {
                /* 让一个完整新样本先经过故障与控制计算，再于下一轮放行MOS。 */
                module->wakeup_fresh_sample_required = 0U;
                module->wakeup_fresh_sample_seen = 0U;
            }
            else
            {
                module->wakeup_fresh_sample_seen = 1U;
            }
        }
        else
        {
            /* 数据/状态无效或序号未更新时继续等待；电流旁路仍由唤醒保持分支判断。 */
            module->wakeup_fresh_sample_seen = 0U;
        }
    }

    /* 上次命令为闭合、旧反馈闭合、新的有效AFE反馈断开：识别DMOS反馈由闭合变断开。 */
    u8DmosFeedbackDropped =
        ((snapshot->afe_status_valid != 0U) &&
         ((module->wakeup_fresh_sample_required == 0U) ||
          (snapshot->afe_current_update_seq != module->wakeup_current_update_seq)) &&
         (module->last_dmos_cmd == APP_MOS_ON) &&
         (module->tstat.u8dsgmos_state != 0U) &&
         (snapshot->mos.dsg_mos_state == 0U)) ? 1U : 0U;

    /* 无效状态快照不能覆盖休眠前保存的MOS反馈缓存。 */
    if (snapshot->afe_status_valid != 0U)
    {
        module->tstat.u8chgmos_state = snapshot->mos.chg_mos_state;
        module->tstat.u8dsgmos_state = snapshot->mos.dsg_mos_state;
    }

    if (module->wakeup_fresh_sample_required != 0U)
    {
        /* 新样本到来前保持MOS；新放电电流优先闭合CMOS，其余按关断条件处理。 */
        return app_h_power_manage_hold_wakeup_outputs(module,
                                                      snapshot,
                                                      &tCurrentRequest,
                                                      u8DmosFeedbackDropped);
    }

    /* 1. 充电 MOS：逐项判断关闭条件，全部满足后打开 CMOS。 */
    app_h_power_manage_cmos_ctrl(module, snapshot, &tCurrentRequest);

    /* 2. 放电允许：先形成统一的放电状态。 */
    app_h_power_manage_dsg_status_judge(module, snapshot, &tCurrentRequest);
    if (u8DmosFeedbackDropped != 0U)
    {
        /* 本轮撤销正常放电资格并复位预放；正电流旁路仍可能在本轮闭合DMOS。 */
        module->tstat.u8dsg_path_allowed = 0U;
    }

    if ((module->tstat.u8dsg_path_allowed != 0U) &&
        (module->last_dmos_cmd == APP_MOS_ON) &&
        (snapshot->afe_status_valid != 0U) &&
        (snapshot->mos.dsg_mos_state != 0U))
    {
        /* DETD在位且已确认DMOS闭合时，保持既有导通，不因电流换向重启预放。 */
        module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_DMOS;
        module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_ON;
    }

    /* 3. 预放 MOS：放电允许后执行预放电状态机。 */
    app_h_power_manage_pdsg_ctrl(module);

    /* 4. 放电MOS：正电流旁路优先于正常路径/预放门控，但受强制关断及通用/PF限制。 */
    app_h_power_manage_dmos_ctrl(module, snapshot, &tCurrentRequest);

    /* 5. 输出下发：端口层负责把业务 enable 转成真实 AFE/GPIO 动作。 */
    eProcessResult = app_h_power_manage_apply_mos(module);
    if (eProcessResult != APP_RESULT_OK)
    {
        return eProcessResult;
    }

    /* 保留输出结构兼容性，但不在功率模块重复下发EFUSE。 */
    module->tcmd.u8efuse_ctl = app_bms_fault_get_efuse_cmd();
    return APP_RESULT_OK;
}

/* 把当前故障汇总转换为 MOS 禁止条件，具体故障位仍由 fault 模块产生。 */
static void app_h_power_manage_build_request(app_h_power_manage_request_t *request,
                                              const struct _app_bms_fault_t *fault)
{
    if ((request == 0) || (fault == 0))
    {
        return;
    }

    request->u8cmos_fault_forbid =
        ((fault->tbms_fault_stat.u8charge_pro_flag != 0U) ||
         (fault->tbms_fault_stat.u8general_fault_flag != 0U) ||
         (fault->tbms_fault_stat.u8PF_fault_flag != 0U)) ? 1U : 0U;

    request->u8dmos_fault_forbid =
        ((fault->tbms_fault_stat.u8discharge_pro_flag != 0U) ||
         (fault->tbms_fault_stat.u8general_fault_flag != 0U) ||
         (fault->tbms_fault_stat.u8PF_fault_flag != 0U)) ? 1U : 0U;
    request->u8dmos_reverse_current_forbid =
        ((fault->tbms_fault_stat.u8general_fault_flag != 0U) ||
         (fault->tbms_fault_stat.u8PF_fault_flag != 0U)) ? 1U : 0U;
}

/* 单包任务路径：保留原请求和原处理函数，未混入并联状态机输出。 */
static void app_h_power_manage_task_single(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    struct _app_bms_fault_t tFaultSnapshot;
    app_h_power_manage_request_t tPowerRequest;

    if (gt_app_h_power_manage.initialized == 0U)
    {
        return;
    }

    app_bms_snapshot_read(&tBmsSnapshot);
    app_bms_fault_snapshot_read(&tFaultSnapshot);

    tPowerRequest = gt_app_h_power_manage.tctl;
    app_h_power_manage_build_request(&tPowerRequest, &tFaultSnapshot);

    (void)app_h_power_manage_process(&gt_app_h_power_manage,
                                     &tBmsSnapshot,
                                     &tPowerRequest);
}

/*
 * 以下为并联专用MOS控制，仅在并联使能时调用。
 * 正常运行的电流优先使用500ms新鲜标志；此处优先级与单包并不完全相同。
 * 放电电流优先闭合CMOS，可覆盖强制关断、故障、并联许可和数据门控。
 */
static void app_h_power_manage_parallel_cmos_ctrl(app_h_power_manage_t *module,
                                                  const app_bms_snapshot_t *snapshot,
                                                  const app_h_power_manage_request_t *request)
{
    int8_t i8ForcedCommand;

    module->tcmd.u8cmos_ctl = 0U;
    module->tstat.u8chg_status = 0U;

    if ((snapshot->discharging != 0U) &&
        (snapshot->afe_current_sample_fresh != 0U))
    {
        module->tcmd.u8cmos_ctl = 1U;
        module->tstat.u8chg_status = 1U;
        return;
    }

    i8ForcedCommand = app_h_power_manage_judge_force(module->tctl.i8cmos_ctl_force, MF_Max);
    if (i8ForcedCommand == APP_MOS_OFF)
    {
        /* 无新鲜放电电流时，最高优先级强制关断生效。 */
        return;
    }

    if ((request->u8cmos_fault_forbid != 0U) &&
        (i8ForcedCommand != APP_MOS_ON))
    {
        /* 充电、通用或PF故障禁止；最高优先级强制开启可覆盖。 */
        return;
    }

    if (module->wakeup_fresh_sample_required != 0U)
    {
        /* 唤醒新样本流程尚未放行，不从此分支新增闭合。 */
        return;
    }

    if ((snapshot->data_ready == 0U) ||
        (module->tctl.i8cmos_ctl_det == APP_MOS_OFF) ||
        (module->tctl.i8cmos_ctl_auth == APP_MOS_OFF))
    {
        /* 数据未就绪、并联检测许可或授权为OFF时，保持CMOS断开。 */
        return;
    }

    /* 强制开关命令仅在并联许可及现有安全门控均允许后执行。 */
    module->tcmd.u8cmos_ctl = 1U;
    module->tstat.u8chg_status = 1U;
}

/* 并联DMOS与单包采用相同的强制关断、故障和预放规则；正电流旁路要求样本新鲜。 */
static void app_h_power_manage_parallel_dmos_ctrl(app_h_power_manage_t *module,
                                                 const app_bms_snapshot_t *snapshot,
                                                 const app_h_power_manage_request_t *request,
                                                 uint8_t u8CurrentSampleFresh)
{
    int8_t i8ForcedCommand;

    module->tcmd.u8dmos_ctl = 0U;

    i8ForcedCommand = app_h_power_manage_judge_force(module->tctl.i8dmos_ctl_force, MF_Max);
    if (i8ForcedCommand == APP_MOS_OFF)
    {
        /* 强制关断优先；放电路径判断会撤销预放资格。 */
        return;
    }

    if ((snapshot->charging != 0U) &&
        (u8CurrentSampleFresh != 0U) &&
        ((request->u8dmos_reverse_current_forbid == 0U) ||
         (i8ForcedCommand == APP_MOS_ON)))
    {
        /* 新鲜正电流可覆盖放电保护；无强制开启时通用/PF故障仍禁止。 */
        module->tcmd.u8dmos_ctl = 1U;
        module->tcmd.u8pdmos_ctl = 0U;
        if ((module->tstat.u8dsg_path_allowed == 0U) ||
            (module->tpdsg.u8step != APP_H_POWER_PDSG_STEP_DMOS))
        {
            /* 未完成的预放在PDMOS断开时不能继续计时；已完成状态则保留。 */
            app_h_power_manage_reset_pdsg(module);
        }
        return;
    }

    if ((request->u8dmos_fault_forbid != 0U) &&
        (i8ForcedCommand != APP_MOS_ON))
    {
        /* DMOS最高优先级强制命令不是开启时，放电保护、通用故障或PF故障禁止闭合。 */
        return;
    }

    if (module->tstat.u8dsg_path_allowed == 0U)
    {
        /* 放电条件不满足，关闭 DMOS。 */
        return;
    }

    if (module->tpdsg.i8dmos_ctl_pdsg != APP_MOS_ON)
    {
        /* 强制打开也不能跳过 DETD 接入后的预放流程。 */
        return;
    }

    /* 放电允许且预放完成，打开 DMOS。 */
    module->tcmd.u8dmos_ctl = 1U;
}

/* 并联唤醒等待：电流旁路使用唤醒后更新的非零序号，此处没有检查500ms新鲜标志。 */
static app_result_t app_h_power_manage_parallel_hold_wakeup_outputs(
    app_h_power_manage_t *module,
    const app_bms_snapshot_t *snapshot,
    const app_h_power_manage_request_t *request,
    uint8_t dmos_feedback_dropped)
{
    int8_t i8CmosForcedCommand;
    int8_t i8DmosForcedCommand;
    int8_t i8PdmosForcedCommand;
    uint8_t u8DischargeCurrentValid;
    uint8_t u8ChargeCurrentValid;
    uint8_t u8DmosMustTurnOff;
    app_result_t eApplyResult;

    u8DischargeCurrentValid = ((snapshot->discharging != 0U) &&
                                (snapshot->afe_current_update_seq != 0UL) &&
                                (snapshot->afe_current_update_seq != module->wakeup_current_update_seq)) ? 1U : 0U;
    u8ChargeCurrentValid = ((snapshot->charging != 0U) &&
                            (snapshot->afe_current_update_seq != 0UL) &&
                            (snapshot->afe_current_update_seq != module->wakeup_current_update_seq)) ? 1U : 0U;

    i8CmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8cmos_ctl_force, MF_Max);
    if (u8DischargeCurrentValid != 0U)
    {
        module->tcmd.u8cmos_ctl = 1U;
    }
    else if ((i8CmosForcedCommand == APP_MOS_OFF) ||
             ((request->u8cmos_fault_forbid != 0U) && (i8CmosForcedCommand != APP_MOS_ON)) ||
             (module->tctl.i8cmos_ctl_det == APP_MOS_OFF) ||
             (module->tctl.i8cmos_ctl_auth == APP_MOS_OFF))
    {
        module->tcmd.u8cmos_ctl = 0U;
    }
    module->tstat.u8chg_status = module->tcmd.u8cmos_ctl;

    i8DmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8dmos_ctl_force, MF_Max);
    i8PdmosForcedCommand = app_h_power_manage_judge_force(module->tctl.i8pdmos_ctl_force, MF_Max);
    if ((u8ChargeCurrentValid != 0U) &&
        (i8DmosForcedCommand != APP_MOS_OFF) &&
        ((request->u8dmos_reverse_current_forbid == 0U) ||
         (i8DmosForcedCommand == APP_MOS_ON)))
    {
        /* 唤醒后新正电流可覆盖放电保护，强制关断和通用/PF故障仍按单包规则处理。 */
        module->tcmd.u8dmos_ctl = 1U;
        module->tcmd.u8pdmos_ctl = 0U;
        u8DmosMustTurnOff = 0U;
    }
    else
    {
        u8DmosMustTurnOff = ((i8DmosForcedCommand == APP_MOS_OFF) ||
                             ((request->u8dmos_fault_forbid != 0U) &&
                              (i8DmosForcedCommand != APP_MOS_ON) &&
                              (i8PdmosForcedCommand != APP_MOS_ON)) ||
                             (module->tctl.i8dmos_ctl_det == APP_MOS_OFF) ||
                             (module->tctl.i8dmos_ctl_auth == APP_MOS_OFF) ||
                             (module->tctl.u8pdsg_enable == 0U) ||
                             (dmos_feedback_dropped != 0U)) ? 1U : 0U;
    }

    if (u8DmosMustTurnOff != 0U)
    {
        module->tcmd.u8dmos_ctl = 0U;
        module->tcmd.u8pdmos_ctl = 0U;
        module->tstat.u8dsg_path_allowed = 0U;
        app_h_power_manage_reset_pdsg(module);
    }
    else if (i8PdmosForcedCommand == APP_MOS_OFF)
    {
        module->tcmd.u8pdmos_ctl = 0U;
        if (module->tcmd.u8dmos_ctl == 0U)
        {
            app_h_power_manage_reset_pdsg(module);
        }
    }

    eApplyResult = app_h_power_manage_apply_mos(module);
    if (eApplyResult != APP_RESULT_OK)
    {
        return eApplyResult;
    }

    module->tcmd.u8efuse_ctl = app_bms_fault_get_efuse_cmd();
    return APP_RESULT_OK;
}

/*
 * 并联专用 MOS 处理流程。
 * 单包仍由 app_h_power_manage_process() 完整处理；此处单独消费并联阶段机计算出的 MOS 许可，
 * 并复用公共的故障快照、远程命令超时、唤醒采样保护、预放/DMOS 安全时序和硬件下发。
 */
static app_result_t app_h_power_manage_process_parallel(app_h_power_manage_t *module,
                                                        const app_bms_snapshot_t *snapshot,
                                                        const app_h_power_manage_request_t *request)
{
    app_result_t eProcessResult;
    app_h_power_manage_request_t tCurrentRequest;
    uint8_t u8DmosFeedbackDropped;

    if ((module == 0) || (snapshot == 0) || (request == 0))
    {
        return APP_RESULT_NULL_PTR;
    }

    if (module->initialized == 0U)
    {
        return APP_RESULT_NOT_INIT;
    }

    tCurrentRequest = *request;
    app_h_power_manage_expire_remote_commands(module, &tCurrentRequest);
    app_h_power_manage_update_pdsg_request(snapshot, &tCurrentRequest);
    module->tctl = tCurrentRequest;

    if (module->wakeup_fresh_sample_required != 0U)
    {
        /* 并联与单包一样，等待数据及AFE状态有效的新序号经过一轮计算。 */
        if ((snapshot->data_ready != 0U) &&
            (snapshot->afe_status_valid != 0U) &&
            (snapshot->afe_current_update_seq != module->wakeup_current_update_seq))
        {
            if (module->wakeup_fresh_sample_seen != 0U)
            {
                module->wakeup_fresh_sample_required = 0U;
                module->wakeup_fresh_sample_seen = 0U;
            }
            else
            {
                module->wakeup_fresh_sample_seen = 1U;
            }
        }
        else
        {
            module->wakeup_fresh_sample_seen = 0U;
        }
    }

    /* 检测上次已闭合的DMOS反馈由闭合变为断开；无效AFE反馈不参与。 */
    u8DmosFeedbackDropped =
        ((snapshot->afe_status_valid != 0U) &&
         ((module->wakeup_fresh_sample_required == 0U) ||
          (snapshot->afe_current_update_seq != module->wakeup_current_update_seq)) &&
         (module->last_dmos_cmd == APP_MOS_ON) &&
         (module->tstat.u8dsgmos_state != 0U) &&
         (snapshot->mos.dsg_mos_state == 0U)) ? 1U : 0U;

    if (snapshot->afe_status_valid != 0U)
    {
        module->tstat.u8chgmos_state = snapshot->mos.chg_mos_state;
        module->tstat.u8dsgmos_state = snapshot->mos.dsg_mos_state;
    }

    if (module->wakeup_fresh_sample_required != 0U)
    {
        return app_h_power_manage_parallel_hold_wakeup_outputs(module,
                                                               snapshot,
                                                               &tCurrentRequest,
                                                               u8DmosFeedbackDropped);
    }

    app_h_power_manage_parallel_cmos_ctrl(module, snapshot, &tCurrentRequest);
    app_h_power_manage_dsg_status_judge(module, snapshot, &tCurrentRequest);
    if (u8DmosFeedbackDropped != 0U)
    {
        module->tstat.u8dsg_path_allowed = 0U;
    }

    if ((module->tstat.u8dsg_path_allowed != 0U) &&
        (module->last_dmos_cmd == APP_MOS_ON) &&
        (snapshot->afe_status_valid != 0U) &&
        (snapshot->mos.dsg_mos_state != 0U))
    {
        /* 已确认DMOS闭合时保持完成状态，回馈及电流换向不触发重新预放。 */
        module->tpdsg.u8step = APP_H_POWER_PDSG_STEP_DMOS;
        module->tpdsg.i8dmos_ctl_pdsg = APP_MOS_ON;
    }

    /* 并联沿用公共预放状态机，正电流旁路仅在未完成时复位预放。 */
    app_h_power_manage_pdsg_ctrl(module);
    /* 并联电流优先仅接受500ms内成功刷新的校准/死区后业务电流样本。 */
    app_h_power_manage_parallel_dmos_ctrl(module,
                                        snapshot,
                                        &tCurrentRequest,
                                        snapshot->afe_current_sample_fresh);

    eProcessResult = app_h_power_manage_apply_mos(module);
    if (eProcessResult != APP_RESULT_OK)
    {
        return eProcessResult;
    }

    module->tcmd.u8efuse_ctl = app_bms_fault_get_efuse_cmd();
    return APP_RESULT_OK;
}

/* 并联任务路径：并联阶段机给出MOS许可，再由并联专用处理函数执行。 */
static void app_h_power_manage_task_parallel(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    struct _app_bms_fault_t tFaultSnapshot;
    app_h_power_manage_request_t tPowerRequest;
    uint8_t u8ParallelReady;
    uint8_t u8KeepExistingDmos;
    int8_t i8DmosForcedCommand;

    if (gt_app_h_power_manage.initialized == 0U)
    {
        return;
    }

    app_bms_snapshot_read(&tBmsSnapshot);
    app_bms_fault_snapshot_read(&tFaultSnapshot);

    tPowerRequest = gt_app_h_power_manage.tctl;
    app_h_power_manage_build_request(&tPowerRequest, &tFaultSnapshot);

    u8ParallelReady = app_parallel_id_is_ready();
    i8DmosForcedCommand = app_h_power_manage_judge_force(
        tPowerRequest.i8dmos_ctl_force,
        MF_Max);

    /*
     * 编址有效性不直接决定DMOS。并联数据暂不可用时，仅沿用当前DMOS闭合命令的许可；
     * 初次接入时DMOS命令仍为关闭，因此不会提前闭合，编址完成后仍走正常预放流程。
     * DETD移除和强制关断仍由本函数及下游HPM安全条件处理，地址冲突时不沿用旧许可。
     */
    u8KeepExistingDmos = ((u8ParallelReady == 0U) &&
                          (tBmsSnapshot.signal.input.mcu_detd != 0U) &&
                          (gtParallelLocalOutput.u8DmosControl != 0U) &&
                          (gt_app_h_power_manage.tcmd.u8dmos_ctl != 0U) &&
                          (i8DmosForcedCommand != APP_MOS_OFF) &&
                          (app_parallel_id_is_address_conflict() == 0U)) ? 1U : 0U;

    if (u8ParallelReady != 0U)
    {
        /* 新编址和并联状态已就绪，使用并联阶段机给出的DMOS许可。 */
        tPowerRequest.i8cmos_ctl_det =
            (gtParallelLocalOutput.u8CmosControl != 0U) ? APP_MOS_ON : APP_MOS_OFF;
        tPowerRequest.i8dmos_ctl_det =
            (gtParallelLocalOutput.u8DmosControl != 0U) ? APP_MOS_ON : APP_MOS_OFF;
    }
    else
    {
        /* 状态未就绪时撤销CMOS检测许可；DMOS按上述条件决定是否沿用许可。 */
        tPowerRequest.i8cmos_ctl_det = APP_MOS_OFF;
        tPowerRequest.i8dmos_ctl_det =
            (u8KeepExistingDmos != 0U) ? APP_MOS_ON : APP_MOS_OFF;
    }

    (void)app_h_power_manage_process_parallel(&gt_app_h_power_manage,
                                              &tBmsSnapshot,
                                              &tPowerRequest);
}

/* 分流入口：并联使能时执行并联路径，否则只执行单包路径。 */
void app_h_power_manage_task(void)
{
    if (gt_app_h_power_manage.initialized == 0U)
    {
        return;
    }

    if (app_parallel_id_is_enabled() != 0U)
    {
        app_parallel_id_task();
        app_h_power_manage_task_parallel();
    }
    else
    {
        app_h_power_manage_task_single();
    }
}

void app_h_power_manage_set_poweroff_override(uint8_t enable)
{
    int8_t command = (enable != 0U) ? APP_MOS_OFF : APP_MOS_IGNORE;

    /* task()从tctl重建请求，因此保存在tctl内的关机强制源会跨周期保持。 */
    gt_app_h_power_manage.tctl.i8cmos_ctl_force[MF_Poweroff] = command;
    gt_app_h_power_manage.tctl.i8dmos_ctl_force[MF_Poweroff] = command;
    gt_app_h_power_manage.tctl.i8pdmos_ctl_force[MF_Poweroff] = command;
}

void app_h_power_manage_set_zs_charge_stop_override(uint8_t enable)
{
    int8_t command = (enable != 0U) ? APP_MOS_OFF : APP_MOS_IGNORE;

    /* 只强制充电MOS；关机来源仍保持更高优先级，10ms任务统一下发。 */
    gt_app_h_power_manage.tctl.i8cmos_ctl_force[MF_ZS_ChargeStop] = command;
}

/* 远程MOS命令使用RTC有效期；到期后释放MF_Remote，恢复自动MOS仲裁。 */
app_result_t app_h_power_manage_set_remote_mos_command(uint8_t u8MosSelect,
                                                        app_mos_cmd_t eMosCommand,
                                                        uint8_t u8DurationMin)
{
    uint32_t u32NowSeconds;
    uint8_t u8Index;
    int8_t *pi8RemoteForce;

    if (gt_app_h_power_manage.initialized == 0U)
    {
        return APP_RESULT_NOT_INIT;
    }

    if ((eMosCommand != APP_MOS_OFF) && (eMosCommand != APP_MOS_ON))
    {
        return APP_RESULT_EXTERNAL_FAIL;
    }

    if (u8MosSelect == APP_H_POWER_MOS_SELECT_PRECHARGE)
    {
        u8Index = APP_H_POWER_REMOTE_PDSG_INDEX;
    }
    else if (u8MosSelect == APP_H_POWER_MOS_SELECT_CHARGE)
    {
        u8Index = APP_H_POWER_REMOTE_CMOS_INDEX;
    }
    else if (u8MosSelect == APP_H_POWER_MOS_SELECT_DISCHARGE)
    {
        u8Index = APP_H_POWER_REMOTE_DMOS_INDEX;
    }
    else
    {
        return APP_RESULT_EXTERNAL_FAIL;
    }

    if (app_h_power_manage_read_rtc_seconds(&u32NowSeconds) == 0U)
    {
        /* RTC不可用于计时就不接受有持续时间的远程控制，避免命令永久生效。 */
        return APP_RESULT_EXTERNAL_FAIL;
    }

    pi8RemoteForce = app_h_power_manage_remote_force_slot(&gt_app_h_power_manage.tctl,
                                                           u8Index);
    if (pi8RemoteForce == 0)
    {
        return APP_RESULT_EXTERNAL_FAIL;
    }

    if (u8DurationMin == 0U)
    {
        u8DurationMin = APP_H_POWER_REMOTE_MOS_DEFAULT_DURATION_MIN;
    }

    *pi8RemoteForce = (int8_t)eMosCommand;
    gt_app_h_power_manage.remote_mos_start_seconds[u8Index] = u32NowSeconds;
    gt_app_h_power_manage.remote_mos_duration_seconds[u8Index] =
        (uint32_t)u8DurationMin * APP_H_POWER_REMOTE_MOS_SECONDS_PER_MINUTE;
    gt_app_h_power_manage.remote_mos_timer_active[u8Index] = 1U;

    return APP_RESULT_OK;
}

/* RTC低功耗巡检使用：到期时离开STOP快路径，恢复任务后撤销远程命令。 */
uint8_t app_h_power_manage_remote_mos_timeout_due(void)
{
    uint32_t u32NowSeconds;
    uint8_t u8Index;

    if ((gt_app_h_power_manage.initialized == 0U) ||
        (app_h_power_manage_remote_timer_active(&gt_app_h_power_manage) == 0U))
    {
        return 0U;
    }

    if (app_h_power_manage_read_rtc_seconds(&u32NowSeconds) == 0U)
    {
        /* RTC读取失败时请求完整唤醒，由运行任务失效关闭远程命令。 */
        return 1U;
    }

    for (u8Index = 0U; u8Index < APP_H_POWER_REMOTE_MOS_COUNT; u8Index++)
    {
        if (gt_app_h_power_manage.remote_mos_timer_active[u8Index] == 0U)
        {
            continue;
        }

        if (app_h_power_manage_remote_command_expired(&gt_app_h_power_manage,
                                                       u8Index,
                                                       u32NowSeconds) != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}

/*
 * 唤醒恢复：先把休眠前命令及下发缓存恢复到内存，等待新样本期间执行保持输出分支；
 * 该函数本身不写硬件。等新数据经过控制计算后恢复正常判断，电流旁路及关断条件仍生效。
 */
app_result_t app_h_power_manage_restore_after_wakeup(
    const app_h_power_manage_output_t *output)
{
    app_bms_snapshot_t snapshot;

    if (output == 0)
    {
        return APP_RESULT_NULL_PTR;
    }

    /* 保存值用于唤醒后保持AFE Sleep期间仍有效的硬件MOS输出。 */
    if ((output->u8cmos_ctl > 1U) ||
        (output->u8dmos_ctl > 1U) ||
        (output->u8pdmos_ctl > 1U))
    {
        return APP_RESULT_EXTERNAL_FAIL;
    }

    /* AFE Sleep会保持MOS状态，唤醒时先保留休眠前命令，避免无效快照触发关断写入。 */
    app_bms_snapshot_read(&snapshot);
    gt_app_h_power_manage.tcmd = *output;
    gt_app_h_power_manage.tstat.u8pdsgmos_state = output->u8pdmos_ctl;
    gt_app_h_power_manage.wakeup_current_update_seq = snapshot.afe_current_update_seq;
    gt_app_h_power_manage.wakeup_fresh_sample_required = 1U;
    gt_app_h_power_manage.wakeup_fresh_sample_seen = 0U;
    gt_app_h_power_manage.last_cmos_cmd = (int8_t)output->u8cmos_ctl;
    gt_app_h_power_manage.last_dmos_cmd = (int8_t)output->u8dmos_ctl;
    gt_app_h_power_manage.last_pdmos_cmd = (int8_t)output->u8pdmos_ctl;

    return APP_RESULT_OK;
}

app_result_t app_h_power_manage_get_output(const app_h_power_manage_t *module,
                                           app_h_power_manage_output_t *output)
{
    if ((module == 0) || (output == 0))
    {
        return APP_RESULT_NULL_PTR;
    }

    *output = module->tcmd;
    return APP_RESULT_OK;
}
