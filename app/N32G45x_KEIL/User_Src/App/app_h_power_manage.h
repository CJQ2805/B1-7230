#ifndef __APP_H_POWER_MANAGE_H__
#define __APP_H_POWER_MANAGE_H__

#include <stdint.h>

#include "App/app_bms_types.h"
#include "Self_Driver/sd_result.h"

/*
 * 文件作用：
 *   APP 层高压/MOS 管理模块公共接口。模块读取统一业务快照，按明确顺序判断
 *   充电 MOS、放电 MOS 和预放 MOS，再通过 Board 端口下发动作。
 *   本头文件只暴露业务层统一类型和调用协议，不依赖 Board、Driver、HSI_afe 或
 *   当前 N32 硬件适配。
 *
 * 输入说明：
 *   1. config：模块级固定配置，只描述 PDSG 延时等静态参数，不保存硬件回调或上下文。
 *   2. port：模块初始化时注入MOS动作和PDSG毫秒时基；远程命令有效期由RTC计时；
 *      回调只表达业务 enable 语义，不暴露 GPIO 电平、AFE 寄存器或板级细节。
 *   3. snapshot：由 app_h_power_manage_task() 从统一业务快照读取。
 *   4. request：由 app_h_power_manage_task() 从当前控制请求和故障汇总组装。
 *
 * 输出说明：
 *   1. output：通过 app_h_power_manage_get_output() 拷出本轮 MOS/efuse 输出；
 *      外部不得获取模块内部输出指针。
 *   2. tstat：模块内部保存的只读运行状态，包含充放电路径允许状态和 MOS 实际反馈。
 *   3. tpdsg：模块内部保存的只读预放电状态，供调试和迁移观察。
 *
 * 调用方法：
 *   1. 系统初始化阶段调用一次 app_h_power_manage_init()。
 *   2. 在 10ms 周期内调用一次 app_h_power_manage_task()。
 *   3. 需要读取输出时，调用 app_h_power_manage_get_output() 把结果写到调用方提供的缓冲区。
 *   4. 低功耗进入/唤醒时调用 prepare_sleep/restore_after_wakeup，保持三路 MOS 状态。
 *
 * 用户需要改哪里：
 *   1. MOS、efuse 或时基落地硬件变化时，请在 Board/Driver/AFE 适配层修改
 *      app_h_power_manage_port_t 对应实现，不要在这里引入板级头文件。
 *   2. 故障汇总变化时，由调用方把禁止条件转换到 request 字段；本模块不读取具体故障对象。
 *
 * 迁移方法：
 *   1. 当前工程通过 gt_app_h_power_manage 观察输入、输出和状态。
 *   2. 新代码优先围绕 app_h_power_manage_t、app_h_power_manage_config_t、
 *      app_h_power_manage_port_t、app_h_power_manage_request_t 和
 *      app_h_power_manage_output_t 使用统一接口。
 *   3. N32 的硬件适配只存在于 Board 端口回调中，本业务模块不包含硬件头文件。
 *
 * 禁止依赖：
 *   1. 本业务头文件禁止包含 Board、Driver、HSI_afe 或 N32 头文件。
 *   2. 业务层禁止把 enable 语义反向解释成具体硬件电平，也禁止在这里暴露 AFE 寄存器位。
 */

/* 应用层 MOS 控制语义：只表达业务请求，不绑定具体硬件极性。 */
typedef enum
{
    APP_MOS_IGNORE = -1, /* 当前条件不参与 MOS 判定。 */
    APP_MOS_OFF = 0,     /* 请求关闭 MOS。 */
    APP_MOS_ON = 1       /* 请求打开 MOS。 */
} app_mos_cmd_t;

/* CAN PF=09使用的MOS选择字节，保持与协议0x01/0x02/0x03一一对应。 */
typedef enum
{
    APP_H_POWER_MOS_SELECT_PRECHARGE = 1U, /* 预放MOS。 */
    APP_H_POWER_MOS_SELECT_CHARGE = 2U,    /* 充电MOS。 */
    APP_H_POWER_MOS_SELECT_DISCHARGE = 3U  /* 放电MOS。 */
} app_h_power_manage_mos_select_t;

/* MOS 控制来源优先级，编号越小优先级越高；关机强制关断最高，远程命令高于一般业务控制。 */
typedef enum
{
    MF_Poweroff = 0,  /* 关机流程强制 MOS，必须高于会请求打开MOS的来源。 */
    MF_Remote,        /* V0.8远程控制优先于保护与一般业务请求。 */
    MF_ZS_ChargeStop, /* 宗申停充完成/超时后强制关闭充电MOS。 */
    MF_Calib,     /* 校准流程强制 MOS。 */
    MF_Cur_Calib, /* 电流校准流程强制 MOS。 */
    MF_ID_Assign, /* 地址或 ID 分配流程强制 MOS。 */
    MF_Max
} MOS_Force_ID_t;

/* 模块固定配置：只保存静态参数，不包含硬件回调、快照或用户上下文。 */
typedef struct
{
    uint32_t pdsg_delay_ms; /* PDSG 从预放开始到允许 DMOS 的延时；填 0 使用默认 300ms。 */
} app_h_power_manage_config_t;

/* MOS 和时基注入入口；enable 均为业务开关语义，硬件极性由端口实现处理。 */
typedef app_result_t (*app_h_power_manage_set_output_fn)(uint8_t enable);
typedef app_result_t (*app_h_power_manage_set_all_output_fn)(uint8_t cmos_enable,
                                                              uint8_t dmos_enable,
                                                              uint8_t pdmos_enable);
typedef uint32_t (*app_h_power_manage_get_time_ms_fn)(void);

/* 硬件端口：独立保存动作回调和上下文，不把硬件接口混入 config。 */
typedef struct
{
    app_h_power_manage_set_output_fn set_charge_mos;     /* 设置充电 MOS，1 打开，0 关闭。 */
    app_h_power_manage_set_output_fn set_discharge_mos;  /* 设置放电 MOS，1 打开，0 关闭。 */
    app_h_power_manage_set_output_fn set_precharge_mos;  /* 设置预放 MOS，1 打开，0 关闭。 */
    app_h_power_manage_set_all_output_fn set_all_mos;    /* 可选：一次性恢复三路 MOS。 */
    app_h_power_manage_set_output_fn set_efuse;          /* 设置二级熔断输出，1 输出有效电平。 */
    app_h_power_manage_get_time_ms_fn get_time_ms;       /* 读取当前毫秒时基，供 PDSG 延时使用。 */
} app_h_power_manage_port_t;

/* 外部业务请求：调用方每个周期填写，模块不读取具体故障对象。 */
typedef struct
{
    uint8_t u8pdsg_enable;            /* PDSG 预放电使能：1 使能，0 禁用。 */

    int8_t i8cmos_ctl_det;            /* 充电检测条件。 */
    int8_t i8cmos_ctl_auth;           /* 充电授权条件。 */
    int8_t i8dmos_ctl_det;            /* 放电检测条件。 */
    int8_t i8dmos_ctl_auth;           /* 放电授权条件。 */

    uint8_t u8cmos_fault_forbid;       /* 充电故障禁止条件；最高优先级远程强制开命令可覆盖。 */
    uint8_t u8dmos_fault_forbid;       /* 放电故障禁止条件；最高优先级远程强制开命令可覆盖。 */

    int8_t i8cmos_ctl_force[MF_Max];  /* 充电 MOS 强制命令数组。 */
    int8_t i8dmos_ctl_force[MF_Max];  /* 放电 MOS 强制命令数组。 */
    int8_t i8pdmos_ctl_force[MF_Max]; /* 预放 MOS 强制命令数组。 */
} app_h_power_manage_request_t;

/* 模块输出：包含三路 MOS和当前 efuse 命令，保持现有观察接口兼容。 */
typedef struct
{
    uint8_t u8cmos_ctl;   /* 本轮计算得到的 CMOS 输出命令。 */
    uint8_t u8dmos_ctl;   /* 本轮计算得到的 DMOS 输出命令。 */
    uint8_t u8pdmos_ctl;  /* 本轮计算得到的 PDMOS 输出命令。 */
    uint8_t u8efuse_ctl;  /* 本轮计算得到的二级熔断输出命令。 */
} app_h_power_manage_output_t;

/* 只读运行状态：包含路径允许状态和 MOS 实际反馈。 */
typedef struct
{
    uint8_t u8chg_status;    /* 充电路径当前是否允许。 */
    uint8_t u8dsg_path_allowed; /* 整个放电路径当前是否允许启动。 */
    uint8_t u8chgmos_state;  /* 充电 MOS 实际反馈状态。 */
    uint8_t u8dsgmos_state;  /* 放电 MOS 实际反馈状态。 */
    uint8_t u8pdsgmos_state; /* 预放 MOS 实际输出状态。 */
} app_h_power_manage_status_t;

/* 只读 PDSG 状态：供调试和故障定位使用。 */
typedef struct
{
    uint8_t u8step;         /* PDSG 状态机步骤。 */
    int8_t i8dmos_ctl_pdsg; /* PDSG 流程给予 DMOS 的允许命令。 */
} app_h_power_manage_pdsg_t;

/*
 * 模块对象：
 *   1. config 保存固定注入配置，port 保存动作端口。
 *   2. request/output/status/pdsg 分别保存输入、输出、运行状态和预放状态。
 *   3. 其余字段是模块内部运行状态；外部仅允许观测，不允许直接改写。
 */
typedef struct _app_h_power_manage_t
{
    app_h_power_manage_config_t config; /* 固定配置。 */
    app_h_power_manage_port_t port;     /* 注入的 MOS 端口。 */
    app_h_power_manage_request_t tctl;  /* 最近一次请求输入。 */
    app_h_power_manage_output_t tcmd;   /* 本轮输出命令。 */
    app_h_power_manage_status_t tstat;  /* 只读运行状态。 */
    app_h_power_manage_pdsg_t tpdsg;   /* 只读 PDSG 状态。 */
    uint32_t pdsg_start_tick_ms;        /* PDSG 起始时间戳。 */
    uint32_t remote_mos_start_seconds[3];    /* 三路RTC日内起始秒数，顺序为PDSG/CMOS/DMOS。 */
    uint32_t remote_mos_duration_seconds[3]; /* 三路远程MOS命令持续时间，单位秒。 */
    uint32_t wakeup_current_update_seq; /* 唤醒时记录的序号，用于确认AFE状态和电流已更新。 */
    int8_t last_cmos_cmd;               /* 上一轮已下发 CMOS 命令缓存。 */
    int8_t last_dmos_cmd;               /* 上一轮已下发 DMOS 命令缓存。 */
    int8_t last_pdmos_cmd;              /* 上一轮已下发 PDMOS 命令缓存。 */
    uint8_t wakeup_fresh_sample_required;/* 等待唤醒后新样本时保持原MOS输出，禁止新增闭合。 */
    uint8_t wakeup_fresh_sample_seen;    /* 新样本已供至少一轮控制/故障计算使用。 */
    uint8_t remote_mos_timer_active[3];  /* 三路远程MOS命令是否正在按RTC计时。 */
    uint8_t initialized;                /* 是否已完成初始化。 */
} app_h_power_manage_t;

/* 兼容声明：现有业务调用仍可直接使用全局对象。 */
extern app_h_power_manage_t gt_app_h_power_manage;

/* 当前工程唯一的初始化入口：获取 Board 端口并把三路 MOS 置于安全关闭状态。 */
app_result_t app_h_power_manage_init(void);

/* 内部处理入口保留为公共声明，便于单元测试直接验证 MOS 判定结果。 */
app_result_t app_h_power_manage_process(app_h_power_manage_t *module,
                                        const app_bms_snapshot_t *snapshot,
                                        const app_h_power_manage_request_t *request);

/* 进入STOP前保存三路MOS输出；唤醒后先保持，已有明确关断条件仍立即生效。 */
void app_h_power_manage_prepare_sleep(app_h_power_manage_output_t *output);

/* 唤醒后恢复保存的三路MOS命令，并等待新AFE样本再恢复常规控制。 */
app_result_t app_h_power_manage_restore_after_wakeup(
    const app_h_power_manage_output_t *output);

/* 当前工程唯一的 10ms 任务入口：读取快照、整理故障并执行三路 MOS 控制。 */
void app_h_power_manage_task(void);

/* 设置关机强制源；enable非0时同时请求关闭充电、放电和预放MOS。 */
void app_h_power_manage_set_poweroff_override(uint8_t enable);

/* 宗申充电流程确认停充或判定充电机无响应后，强制关闭/释放充电MOS命令。 */
void app_h_power_manage_set_zs_charge_stop_override(uint8_t enable);

/*
 * CAN等远程协议的唯一MOS命令入口：只写最高业务优先级的MF_Remote请求，实际下发仍在
 * 10ms任务中经过数据、检测和端口门控。保护故障不屏蔽该远程请求；关机强制关断仍优先。
 * u8DurationMin为1字节分钟数（0按默认1分钟，最大255分钟）；命令到期后恢复自动仲裁。
 * u8MosSelect只能是PRECHARGE/CHARGE/DISCHARGE，eMosCommand只能是APP_MOS_OFF或APP_MOS_ON。
 */
app_result_t app_h_power_manage_set_remote_mos_command(uint8_t u8MosSelect,
                                                        app_mos_cmd_t eMosCommand,
                                                        uint8_t u8DurationMin);

/* 低功耗RTC巡检调用：远程MOS控制超时后应恢复任务调度，以便应用新的自动仲裁结果。 */
uint8_t app_h_power_manage_remote_mos_timeout_due(void);

app_result_t app_h_power_manage_get_output(const app_h_power_manage_t *module,
                                           app_h_power_manage_output_t *output);

#endif
