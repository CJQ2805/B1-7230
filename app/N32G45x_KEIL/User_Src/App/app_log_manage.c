/*
 * 文件作用：
 *   APP层LOG管理模块，负责故障变化、看门狗复位和软关机快照的记录、读取和导出数据准备。
 *
 * 主要用途：
 *   1. 生成运行LOG记录，记录故障、告警、复位原因、MOS状态、电压、电流、温度等快照。
 *   2. 调用Flash均衡擦写中间件保存LOG，支持掉电后续写。
 *   3. 给通信模块提供LOG数量和按索引读取接口。
 *
 * 适用范围：
 *   适用于当前BMS APP的片内Flash LOG记录。后续模块化改造时，本模块应逐步改为
 *   只消费统一业务快照和各模块公开输出。
 *
 * 调用关系：
 *   SaveLog_Task每10ms调用 app_log_task()；通信模块调用读取接口导出LOG；底层存储由 m_flash_wl 完成。
 *
 * 注意事项：
 *   LOG写入不能阻塞主任务太久；结构体长度要和底层槽位长度保持可追溯关系。
 */
#include "app_log_manage.h"        /* LOG结构体、触发原因和对外接口声明。 */
#include "App/app_flash_map.h"     /* APP侧Flash分区唯一地址定义。 */
#include "app_bms_fault.h"      /* 读取BMS故障模块的最终故障图。 */
#include "app_bms_fault_config.h" /* 复用NTC故障数量阈值，避免日志和保护使用不同口径。 */
#include "app_bms_snapshot.h"      /* 读取统一业务快照，避免LOG重复拼装采样数据。 */
#include "App/app_param.h"          /* 读取项目级拓扑配置。 */
#include "Driver/d_timer.h"    /* 读取系统运行时间，用于LOG时间戳。 */
#include "Middle/m_flash_wl.h"     /* 使用Flash均衡擦写模块保存LOG。 */
#include "Driver/d_rtc.h"     /* 优先使用RTC真实时间作为LOG时间戳。 */
#include "Driver/d_system.h"  /* 读取复位来源和当前系统主频。 */
#include "RTT/SEGGER_RTT.h"   /* 启动早期输出硬件复位来源。 */
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include <string.h>                /* 使用memset清空快照结构体。 */

/*
 * LOG整体流程：
 * 1. main初始化时调用app_log_init()，扫描Flash均衡擦写区域，确认LOG能不能写。
 * 2. BMS故障位发生置位或清除时，app_bms_fault.c调用app_log_request_snapshot()；
 *    检测到非纯上电复位时，app_log_init()自动入队一条独立复位记录。
 * 3. app_log_request_snapshot()只生成RAM快照并放入队列，不直接写Flash。
 * 4. SaveLog_Task每10ms调用app_log_task()，输出NTC诊断变化。
 * 5. app_log_task()每次最多把一条RAM快照写入Flash，避免单次处理积压过多。
 */

/* LOG数据库最后64K中，先预留2K给后续索引/版本信息，实时快照使用后面48K。 */
#define APP_LOG_RTD_PAGE_COUNT          APP_FLASH_MAP_LOG_RTD_PAGE_COUNT /* 与SOC存储分区共用唯一页数定义，防止分区重叠。 */

/* 队列只放RAM快照，Flash写入放到SaveLog_Task的10ms周期中分批消费。 */
#define APP_LOG_PENDING_NUM             20U      /* RAM中最多缓存20条等待写入的快照。 */
#define APP_LOG_INVALID_CELL_MV         6000U    /* 单体电压大于6000mV认为异常，不参与最大/最小值计算。 */
#define APP_LOG_U16_MAX                 65535U   /* uint16_t最大值，用于初始化最小电压。 */
#define APP_LOG_I16_MAX                 32767    /* int16_t最大值，用于限制电流范围。 */
#define APP_LOG_I16_MIN                 (-32768) /* int16_t最小值，用于限制电流范围。 */

/* 实时故障快照对应的Flash均衡擦写配置。 */
static const m_flash_wl_cfg_t s_log_rtd_cfg =
{
    APP_FLASH_MAP_LOG_RTD_START_ADDR, /* start_addr：告诉底层从统一Flash分区地址开始存。 */
    APP_LOG_RTD_PAGE_COUNT,  /* page_count：告诉底层这个分区一共有多少页。 */
    LOG_RTD_STORE_SIZE,      /* record_size：每条记录在Flash中的固定槽位大小。 */
    (uint8_t)LOG_RECORD_RTD  /* area_id：区分这是实时故障快照区域。 */
};

/* Flash均衡擦写运行状态，底层会在这里记录当前写指针、最新记录和页序号。 */
static m_flash_wl_area_t s_log_rtd_area;

/*
 * 调试重点：
 * s_log_ready = 1 表示LOG Flash区初始化成功，可以接收并写入快照。
 * s_log_pending_count 表示当前RAM里还有几条快照等待写入Flash。
 * s_log_overflow_count 表示队列满后丢弃旧快照的次数，导出LOG时可用于判断是否丢记录。
 */
static log_rtd_t s_log_pending[APP_LOG_PENDING_NUM]; /* RAM快照环形队列，保存还没写入Flash的LOG。 */
static uint8_t s_log_pending_head;                   /* 队列写入位置，下次新快照放这里。 */
static uint8_t s_log_pending_tail;                   /* 队列读取位置，下次写Flash取这里。 */
static uint8_t s_log_pending_count;                  /* 队列中当前有效快照数量。 */
static volatile uint8_t s_log_ready;                 /* LOG模块可写标志，Flash初始化失败时为0。 */
static uint8_t s_log_overflow_count;                 /* 队列溢出计数，用来标记是否丢过旧快照。 */
static uint8_t s_log_ntc_diag_inited;                /* NTC诊断基准是否已建立。 */
static uint8_t s_log_last_ntc_fault_mask;             /* 上一次输出诊断时的NTC原始故障掩码。 */
static uint8_t s_log_last_ntc_count;                  /* 上一次输出诊断时的异常数量。 */
static d_system_reset_flags_t s_log_reset_flags;      /* 启动早期捕获的复位来源，仅写入独立复位记录。 */
static uint8_t s_log_reset_flags_pending;             /* 尚未成功保存的本次启动复位来源。 */
static uint8_t s_log_reset_flags_captured;             /* 标记硬件复位标志是否已在main早期读取并清除。 */
static SemaphoreHandle_t s_log_mutex;                /* 串行化跨任务LOG入队、写入和flush状态读取。 */

static const char *app_log_trigger_name(uint8_t trigger);

/* main初始化阶段单线程调用LOG接口；运行期通过递归互斥锁保护队列。 */
static bool app_log_lock(TickType_t timeout_ticks)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return true;
    }

    return ((s_log_mutex != NULL) &&
            (xSemaphoreTakeRecursive(s_log_mutex, timeout_ticks) == pdTRUE));
}

static void app_log_unlock(void)
{
    if ((xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) &&
        (s_log_mutex != NULL))
    {
        (void)xSemaphoreGiveRecursive(s_log_mutex);
    }
}

/* 把32位计算结果限制到int16_t范围，防止电流等数据溢出LOG结构体字段。 */
static int16_t app_log_limit_i16(int32_t value)
{
    if (value > APP_LOG_I16_MAX) /* 如果输入值超过int16_t正向最大值。 */
    {
        return APP_LOG_I16_MAX;  /* 返回int16_t最大值，避免强制转换后溢出。 */
    }

    if (value < APP_LOG_I16_MIN) /* 如果输入值低于int16_t负向最小值。 */
    {
        return APP_LOG_I16_MIN;  /* 返回int16_t最小值，避免强制转换后溢出。 */
    }

    return (int16_t)value;       /* 输入值在范围内，直接转换成int16_t。 */
}

/* 优先填入RTC真实时间；如果RTC还没有被写入过有效时间，就退回保存上电后的运行时分秒。 */
static void app_log_fill_uptime(log_rtd_t *log)
{
    d_rtc_time_t tRtcTime; /* 临时保存RTC读出的年月日时分秒。 */
    uint32_t u32TotalSeconds = d_timer_get_ms() / 1000U; /* 把系统运行毫秒数换算成总秒数。 */

    if (d_rtc_read(&tRtcTime)) /* RTC已经初始化并且写入过有效时间时，LOG直接使用真实时间。 */
    {
        log->year = (uint8_t)(tRtcTime.year - 2000U); /* LOG结构体年份只保存2000年后的偏移值。 */
        log->month = tRtcTime.month;                  /* 保存月份。 */
        log->day = tRtcTime.day;                      /* 保存日期。 */
        log->hour = tRtcTime.hour;                    /* 保存小时。 */
        log->min = tRtcTime.min;                      /* 保存分钟。 */
        log->sec = tRtcTime.sec;                      /* 保存秒。 */
        return;                                       /* 已经拿到真实时间，不再使用运行秒数。 */
    }

    log->hour = (uint8_t)((u32TotalSeconds / 3600U) % 24U); /* 保存运行时间中的小时，超过24小时重新从0开始。 */
    log->min  = (uint8_t)((u32TotalSeconds / 60U) % 60U);   /* 保存运行时间中的分钟。 */
    log->sec  = (uint8_t)(u32TotalSeconds % 60U);           /* 保存运行时间中的秒。 */
}

/* 采集外部信号和MOS状态，仅作为其他事件触发日志时的快照内容。 */
static log_ex_sig_t app_log_collect_ex_sig(const app_bms_snapshot_t *snapshot)
{
    log_ex_sig_t ex_sig; /* 临时保存本次采集到的外部信号/MOS状态。 */

    ex_sig.ex_sig = 0UL; /* 先清零所有bit，避免未使用bit留下脏数据。 */
    if (snapshot == NULL)
    {
        return ex_sig;
    }

    ex_sig.bit.chg_mos_s = (snapshot->mos.chg_mos_state != 0U) ? 1U : 0U;  /* 充电MOS实际反馈状态。 */
    ex_sig.bit.dsg_mos_s = (snapshot->mos.dsg_mos_state != 0U) ? 1U : 0U;  /* 放电MOS实际反馈状态。 */
    ex_sig.bit.pre_mos_s = (snapshot->mos.pre_mos_state != 0U) ? 1U : 0U; /* 预放MOS实际输出状态。 */
    /* 使用统一快照中的已消抖板级输入；1=在位，避免旧版硬编码0导致日志丢失检测状态。 */
    ex_sig.bit.detc_stu  = (snapshot->signal.input.mcu_detc != 0U) ? 1U : 0U;
    ex_sig.bit.detd_stu  = (snapshot->signal.input.mcu_detd != 0U) ? 1U : 0U;

    return ex_sig; /* 返回本次采集结果，供快照使用。 */
}

/* 正常POR不单独存LOG；POR同时带PINRST且没有其他原因时也按正常上电处理。 */
static uint8_t app_log_has_recordable_reset(void)
{
    return (uint8_t)((s_log_reset_flags.bor_reset != 0U) ||
                     (s_log_reset_flags.soft_reset != 0U) ||
                     (s_log_reset_flags.iwdg_reset != 0U) ||
                     (s_log_reset_flags.wwdg_reset != 0U) ||
                     (s_log_reset_flags.lpwr_reset != 0U) ||
                     ((s_log_reset_flags.pin_reset != 0U) &&
                      (s_log_reset_flags.por_reset == 0U)));
}

/* 仅把本次RCC复位标志转换到专用复位记录中。 */
static log_reset_s_t app_log_collect_reset_source(uint8_t trigger)
{
    log_reset_s_t reset_s; /* 临时保存转换后的复位来源。 */

    reset_s.reset_s = 0U; /* 先清零所有复位来源bit。 */
    if ((trigger != (uint8_t)LOG_TRIGGER_RESET) || (s_log_reset_flags_pending == 0U))
    {
        return reset_s; /* 普通告警/故障快照不携带复位来源。 */
    }

    reset_s.bit.v18pwr_rstf = s_log_reset_flags.bor_reset;  /* BOR欠压复位标志。 */
    reset_s.bit.pin_rst     = s_log_reset_flags.pin_reset;  /* NRST引脚复位标志。 */
    reset_s.bit.por_rst     = s_log_reset_flags.por_reset;  /* 上电/掉电复位标志。 */
    reset_s.bit.sft_rst     = s_log_reset_flags.soft_reset; /* 软件复位标志。 */
    reset_s.bit.iwd_rst     = s_log_reset_flags.iwdg_reset; /* 独立看门狗复位标志。 */
    reset_s.bit.wwd_rst     = s_log_reset_flags.wwdg_reset; /* 窗口看门狗复位标志。 */
    reset_s.bit.lpwr_rst    = s_log_reset_flags.lpwr_reset; /* 低功耗复位标志。 */

    return reset_s; /* 返回转换后的复位来源。 */
}

/* 采集DVC1124硬件故障状态，来自AFE状态寄存器解析结果。 */
static log_hw_err_t app_log_collect_hw_err(const app_bms_snapshot_t *snapshot)
{
    log_hw_err_t hw_err; /* 临时保存AFE硬件故障状态。 */

    hw_err.status = 0U; /* 先清零所有硬件故障bit。 */
    if (snapshot == NULL)
    {
        return hw_err;
    }

    hw_err.bit.sc     = (snapshot->afe_fault.short_circuit != 0U) ? 1U : 0U; /* AFE短路保护标志。 */
    hw_err.bit.dsgoc2 = (snapshot->afe_fault.dsg_oc2 != 0U) ? 1U : 0U;       /* AFE二级放电过流标志。 */
    hw_err.bit.dsgoc1 = (snapshot->afe_fault.dsg_oc1 != 0U) ? 1U : 0U;       /* AFE一级放电过流标志。 */
    hw_err.bit.celluv = (snapshot->afe_fault.cell_uv != 0U) ? 1U : 0U;       /* AFE欠压保护标志。 */
    hw_err.bit.cellov = (snapshot->afe_fault.cell_ov != 0U) ? 1U : 0U;       /* AFE过压保护标志。 */
    hw_err.bit.chgoc  = ((snapshot->afe_fault.chg_oc1 != 0U) ||
                         (snapshot->afe_fault.chg_oc2 != 0U)) ? 1U : 0U;       /* AFE充电过流标志。 */

    return hw_err; /* 返回本次采集到的AFE硬件故障。 */
}

/* 采集软件告警状态，只保存Warning类状态，不和Protection/Lock故障合并。 */
static log_sw_warn_t app_log_collect_sw_warn(const app_bms_snapshot_t *snapshot)
{
    struct _app_bms_fault_t fault_snapshot;
    const t_bms_fault_flag *fault = &fault_snapshot.tbms_fault_flag; /* 使用最终故障图复制，避免读取半更新结构。 */
    log_sw_warn_t sw_warn;                                              /* 临时保存转换后的LOG软件告警图。 */

    app_bms_fault_snapshot_read(&fault_snapshot);

    sw_warn.status = 0UL; /* 先清零所有告警bit，未使用的告警源保持0。 */

    /* 放电过流告警统一保存到一个告警位，旧的第二个位置固定为0。 */
    sw_warn.bit.dsgoc1 = fault->udischarge_fault_A.tdischarge_fault_A.b1pack_war_ocd; /* 统一放电过流告警。 */
    sw_warn.bit.dsgoc2 = 0U; /* 兼容旧日志结构，当前不再表示独立告警等级。 */

    /* 放电温度和欠压告警：新记录区分单体欠压与总压欠压。 */
    sw_warn.bit.dsgot = fault->udischarge_fault_A.tdischarge_fault_A.b1pack_war_otd; /* 放电高温告警。 */
    sw_warn.bit.dsgut = fault->udischarge_fault_A.tdischarge_fault_A.b1pack_war_utd; /* 放电低温告警。 */
    sw_warn.bit.celluv = fault->udischarge_fault_A.tdischarge_fault_A.b1cell_war_cuv; /* 单体欠压告警。 */
    sw_warn.bit.packuv = fault->udischarge_fault_B.tdischarge_fault_B.b1pack_war_cuv; /* 总压欠压告警。 */

    /* 充电电流、温度和过压告警：新记录区分普通充电与回馈过流。 */
    sw_warn.bit.chgoc = fault->ucharge_fault_A.tcharge_fault_A.b1pack_war_occ; /* 普通充电过流告警。 */
    sw_warn.bit.recharge_oc = fault->ucharge_fault_A.tcharge_fault_A.b1pack_war_orcc; /* 回馈过流告警。 */
    sw_warn.bit.chgot = fault->ucharge_fault_A.tcharge_fault_A.b1pack_war_otc;             /* 充电高温告警。 */
    sw_warn.bit.chgut = fault->ucharge_fault_A.tcharge_fault_A.b1pack_war_utc;             /* 充电低温告警。 */
    sw_warn.bit.cellov1 = fault->ucharge_fault_A.tcharge_fault_A.b1cell_war_cov; /* 单体过压告警。 */
    sw_warn.bit.packov = fault->ucharge_fault_B.tcharge_fault_B.b1pack_war_pov; /* 总压过压告警。 */

    /* 通用告警：只收集waring位。 */
    sw_warn.bit.unbalance = fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_waring; /* 单体压差告警。 */
    sw_warn.bit.mos_ot = fault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_waring;              /* MOS过温告警。 */
    sw_warn.bit.ntc_warn = ((snapshot != NULL) &&
                            (snapshot->ntc_abnormal_count >= 1U) &&
                            (snapshot->ntc_abnormal_count <= (BMS_NTC_FAULT_MIN_COUNT - 1U))) ? 1U : 0U; /* 至少1路异常且未达到故障数量阈值时只告警。 */

    return sw_warn; /* 返回本次采集到的软件告警图。 */
}

/* 采集软件故障状态，只保存pro/fault/lock/fuse类状态，不和告警合并。 */
static log_sw_err_t app_log_collect_sw_err(const app_bms_snapshot_t *snapshot)
{
    struct _app_bms_fault_t fault_snapshot;
    const t_bms_fault_flag *fault = &fault_snapshot.tbms_fault_flag; /* 使用最终故障图复制。 */
    const t_bms_fault_stat *fault_stat = &fault_snapshot.tbms_fault_stat; /* 使用同一时刻的汇总保护状态。 */
    log_sw_err_t sw_err;                                                /* 临时保存转换后的LOG软件故障图。 */

    app_bms_fault_snapshot_read(&fault_snapshot);

    sw_err.status = 0UL; /* 先清零所有软件故障bit。 */

    /* 放电过流故障：只保存保护和锁定，不保存war告警。 */
    sw_err.bit.dsgoc1 = fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd1; /* 一级放电过流保护。 */
    sw_err.bit.dsgoc2 = (fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd2 ||
                         fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd3 ||
                         fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_ocd4 ||
                         fault->udischarge_fault_A.tdischarge_fault_A.b1pack_ocd_lock) ? 1U : 0U; /* 二级及以上保护或锁定。 */

    /* 放电温度、短路和欠压类故障。 */
    sw_err.bit.dsgot = fault->udischarge_fault_A.tdischarge_fault_A.b1pack_pro_otd; /* 放电高温保护。 */
    sw_err.bit.dsgut = fault->udischarge_fault_A.tdischarge_fault_A.b1pack_pro_utd; /* 放电低温保护。 */
    sw_err.bit.sc = fault->udischarge_fault_B.tdischarge_fault_B.b1pack_shrot; /* 电池包短路保护。 */
    sw_err.bit.celluv = (fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv1 ||
                         fault->udischarge_fault_A.tdischarge_fault_A.b1cell_pro_cuv2 ||
                         fault->udischarge_fault_B.tdischarge_fault_B.b1pack_pro_cuv) ? 1U : 0U; /* 单体或总压欠压保护。 */
    sw_err.bit.celluv2 = fault->udischarge_fault_A.tdischarge_fault_A.b1cell_deep_cuv; /* 深度欠压锁存。 */

    /* 充电过流、温度和过压类故障。 */
    sw_err.bit.chgoc = (fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ1 ||
                        fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_occ2 ||
                        fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_orcc ||
                        fault->ucharge_fault_B.tcharge_fault_B.b1pack_occ_lock) ? 1U : 0U; /* 充电过流保护、反向电流保护或过流锁定。 */
    sw_err.bit.chgot = fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_otc; /* 充电高温保护。 */
    sw_err.bit.chgut = fault->ucharge_fault_A.tcharge_fault_A.b1pack_pro_utc; /* 充电低温保护。 */
    sw_err.bit.cellov1 = (fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov ||
                          fault->ucharge_fault_B.tcharge_fault_B.b1pack_pro_pov) ? 1U : 0U; /* 单体或总压过压保护。 */
    sw_err.bit.cellov2 = fault->ucharge_fault_A.tcharge_fault_A.b1cell_pro_cov2; /* 二级单体过压保护。 */
    sw_err.bit.ovpf = 0U; /* 当前故障结构没有独立OVPF来源，不再重复映射二级过压。 */

    /* 通用故障：MOS失效、压差、过温、熔断、AFE异常等。 */
    sw_err.bit.chgmosfail = fault->ugeneral_fault_A.tgeneral_fault_A.b1CHG_MOS_failure; /* 充电MOS失效。 */
    sw_err.bit.dsgmosfail = fault->ugeneral_fault_A.tgeneral_fault_A.b1DSG_MOS_faulure; /* 放电MOS失效。 */
    sw_err.bit.badcell = 0U; /* 当前没有独立坏单体来源，不与压差保护重复记录。 */
    sw_err.bit.unbalance = (fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault ||
                            fault->ugeneral_fault_A.tgeneral_fault_A.b1cell_voltage_diff_fault2) ? 1U : 0U; /* 单体压差一级/二级保护。 */
    sw_err.bit.chg_pro = (fault_stat->u8charge_pro_flag != 0U) ? 1U : 0U; /* 充电保护汇总，协议Bit17。 */
    sw_err.bit.dsg_pro = (fault_stat->u8discharge_pro_flag != 0U) ? 1U : 0U; /* 放电保护汇总，协议Bit18。 */
    sw_err.bit.mos_ot = fault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP; /* MOS过温保护。 */
    sw_err.bit.fuse_on_f = (fault->ugeneral_fault_A.tgeneral_fault_A.b1SCP_FUSE_fusing ||
                            fault->ugeneral_fault_A.tgeneral_fault_A.b1MOS_OTP_FUSE ||
                            fault->ugeneral_fault_A.tgeneral_fault_A.b1OTD_FUSE) ? 1U : 0U; /* 熔断相关触发条件。 */
    sw_err.bit.afe_err_f = fault->ugeneral_fault_A.tgeneral_fault_A.b1AFE_fault; /* 只记录AFE通信/状态故障，NTC故障单独记录到ntc_fault。 */
    sw_err.bit.ntc_fault = ((snapshot != NULL) &&
                            (snapshot->ntc_abnormal_count >= BMS_NTC_FAULT_MIN_COUNT)) ? 1U : 0U; /* 达到配置数量阈值时记录NTC故障。 */

    return sw_err; /* 返回转换后的软件故障图。 */
}

/* 业务层简单累加校验，底层均衡擦写模块还会再保存CRC。 */
static uint8_t app_log_calc_checksum(const log_rtd_t *log)
{
    const uint8_t *pu8LogBytes = (const uint8_t *)log; /* 把结构体看成连续字节，方便逐字节累加。 */
    uint16_t u16ByteIndex;                            /* 循环下标。 */
    uint8_t u8Checksum = 0U;                          /* 校验累加值从0开始。 */

    for (u16ByteIndex = 0U; u16ByteIndex < (uint16_t)(sizeof(log_rtd_t) - 1U); u16ByteIndex++) /* 最后1字节就是checksum自身，不参与计算。 */
    {
        u8Checksum = (uint8_t)(u8Checksum + pu8LogBytes[u16ByteIndex]); /* 把当前字节累加进去，自然保留低8位。 */
    }

    return u8Checksum; /* 返回计算完成的业务层校验值。 */
}

/* 采集单体电压摘要：最大/最小电压、对应编号和电池包总压。 */
static void app_log_collect_cell_data(log_rtd_t *log, const app_bms_snapshot_t *snapshot)
{
    uint8_t u8CellIndex;                         /* 单体循环下标。 */
    uint8_t u8CellCount;                          /* 优先使用统一快照给出的实际串数。 */
    uint8_t u8ValidCellCount = 0U;                /* 有效单体数量，过滤0和异常值后累加。 */
    uint32_t u32PackVoltageSumMv = 0UL;           /* 单体电压求和，VBAT没采到时用于估算总压。 */
    uint16_t u16MinCellVoltageMv = APP_LOG_U16_MAX; /* 最小电压先放最大值，方便第一次有效电压覆盖。 */
    uint16_t u16MaxCellVoltageMv = 0U;            /* 最大电压先放0，方便第一次有效电压覆盖。 */
    uint8_t u8MinCellNumber = 0U;                 /* 最低电压单体编号，LOG中按1开始编号。 */
    uint8_t u8MaxCellNumber = 0U;                 /* 最高电压单体编号，LOG中按1开始编号。 */

    if ((log == NULL) || (snapshot == NULL))
    {
        return;
    }

    u8CellCount = snapshot->cell_count;

    if ((u8CellCount == 0U) || (u8CellCount > APP_BMS_CELL_COUNT)) /* 如果逻辑串数未初始化，或超过快照数组长度。 */
    {
        u8CellCount = APP_BMS_CELL_COUNT; /* 按当前项目逻辑串数扫描，避免把AFE物理通道空位当作电芯。 */
    }

    for (u8CellIndex = 0U; u8CellIndex < u8CellCount; u8CellIndex++) /* 从第1节到最后一节逐个扫描单体电压。 */
    {
        uint16_t u16CellVoltageMv = snapshot->cell_mv[u8CellIndex]; /* 取出当前单体电压，单位按mV处理。 */

        if ((u16CellVoltageMv == 0U) || (u16CellVoltageMv > APP_LOG_INVALID_CELL_MV)) /* 过滤未采到或明显不合理的电压。 */
        {
            continue; /* 当前单体无效，跳过最大/最小值和总压统计。 */
        }

        u32PackVoltageSumMv += u16CellVoltageMv; /* 把有效单体电压累加，用于后面估算总压。 */
        u8ValidCellCount++;          /* 有效单体数量加1。 */

        if (u16CellVoltageMv > u16MaxCellVoltageMv) /* 如果当前单体比之前记录的最高电压还高。 */
        {
            u16MaxCellVoltageMv = u16CellVoltageMv;             /* 更新最高单体电压。 */
            u8MaxCellNumber = (uint8_t)(u8CellIndex + 1U);  /* 更新最高单体编号，外部显示按1开始。 */
        }

        if (u16CellVoltageMv < u16MinCellVoltageMv) /* 如果当前单体比之前记录的最低电压还低。 */
        {
            u16MinCellVoltageMv = u16CellVoltageMv;             /* 更新最低单体电压。 */
            u8MinCellNumber = (uint8_t)(u8CellIndex + 1U);  /* 更新最低单体编号，外部显示按1开始。 */
        }
    }

    if (u8ValidCellCount != 0U) /* 至少采到一个有效单体，才把最大/最小值写入LOG。 */
    {
        log->maxvol = u16MaxCellVoltageMv;          /* 保存最高单体电压。 */
        log->minvol = u16MinCellVoltageMv;          /* 保存最低单体电压。 */
        log->max_cell_num = u8MaxCellNumber;   /* 保存最高单体编号。 */
        log->min_cell_num = u8MinCellNumber;   /* 保存最低单体编号。 */
    }

    if (snapshot->vbat_mv != 0UL) /* 如果AFE已经采到电池包总压。 */
    {
        log->packvol = (uint16_t)(snapshot->vbat_mv / 100UL); /* 快照VBAT按mV处理，LOG中packvol单位是0.1V。 */
    }
    else /* 如果AFE总压还没采到。 */
    {
        log->packvol = (uint16_t)(u32PackVoltageSumMv / 100UL); /* 用单体求和估算总压，同样换成0.1V。 */
    }
}

/* 按当前系统状态生成一条完整快照；这里只填RAM结构，不直接写Flash。 */
static void app_log_build_snapshot(log_rtd_t *log, log_trigger_t trigger)
{
    app_bms_snapshot_t tBmsSnapshot;

    memset(log, 0, sizeof(*log)); /* 先清空整条快照，未使用字段保持0。 */

    app_bms_snapshot_read(&tBmsSnapshot); /* 统一读取电压、电流、温度和NTC摘要。 */

    app_log_fill_uptime(log);     /* 填入上电运行时分秒。 */
    app_log_collect_cell_data(log, &tBmsSnapshot);/* 填入单体最大/最小电压、编号和总压。 */

    log->soc = tBmsSnapshot.soc.u8display_soc; /* 保存当前显示SOC，单位%。 */
    log->cur = app_log_limit_i16(tBmsSnapshot.current_ma / 10); /* 保存校准后实际电流，不能使用带1A死区的current_10ma。 */
    log->max_cell_temp = tBmsSnapshot.max_temp_c; /* 保存AFE_NTC、CELL_NTC1、CELL_NTC2中的最高电芯温度。 */
    log->min_cell_temp = tBmsSnapshot.min_temp_c; /* 保存AFE_NTC、CELL_NTC1、CELL_NTC2中的最低电芯温度。 */
    if (tBmsSnapshot.temp_count != 0U)
    {
        log->max_temp_num = (uint8_t)(tBmsSnapshot.max_temp_num + 1U); /* 对外编号从1开始。 */
        log->min_temp_num = (uint8_t)(tBmsSnapshot.min_temp_num + 1U); /* 对外编号从1开始。 */
    }
    log->mos_temp = ((tBmsSnapshot.ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U) ?
                    tBmsSnapshot.ntc.mos_temp_0p1c : (int16_t)0xFFFF; /* MOS_NTC无效时按协议保留0xFFFF。 */
    log->chg_port_vol = (uint16_t)(tBmsSnapshot.bat_mv / 100UL); /* 当前无独立端口采样，沿用单体电压累加总压，单位0.1V。 */
    log->dsg_port_vol = (uint16_t)(tBmsSnapshot.bat_mv / 100UL); /* 当前无独立端口采样，沿用单体电压累加总压，单位0.1V。 */
    log->hw_err = app_log_collect_hw_err(&tBmsSnapshot);  /* 填入AFE硬件故障快照。 */
    log->sw_warn = app_log_collect_sw_warn(&tBmsSnapshot); /* 用同一份快照生成软件告警。 */
    log->sw_err = app_log_collect_sw_err(&tBmsSnapshot);   /* 用同一份快照生成软件故障。 */
    if (trigger == LOG_TRIGGER_RESET)
    {
        /* 复位来源单独成条；本行不混入同一启动阶段的告警/故障状态。 */
        log->hw_err.status = 0U;
        log->sw_warn.status = 0UL;
        log->sw_err.status = 0UL;
    }
    log->reserved_store[0] = LOG_WARN_MAP_VERSION_V1; /* 供读日志协议区分新旧告警位语义，保持旧记录可解码。 */
    if (tBmsSnapshot.ntc_abnormal_count != 0U)
    {
        SEGGER_RTT_printf(0,
                          "LOG_NTC_CAPTURE reason=%s(%u) count=%u fault[AFE=1,CELL1=2,CELL2=4,MOS=8,PTC=16]=0x%02X valid=0x%02X short=0x%02X open=0x%02X raw[cell1=%u cell2=%u mos=%u ptc=%u]\r\n",
                          app_log_trigger_name((uint8_t)trigger),
                          (unsigned int)trigger,
                          (unsigned int)tBmsSnapshot.ntc_abnormal_count,
                          (unsigned int)tBmsSnapshot.ntc.fault_mask,
                          (unsigned int)tBmsSnapshot.ntc.valid_mask,
                          (unsigned int)tBmsSnapshot.ntc.short_mask,
                          (unsigned int)tBmsSnapshot.ntc.open_mask,
                          (unsigned int)tBmsSnapshot.ntc.cell_ntc1_raw,
                          (unsigned int)tBmsSnapshot.ntc.cell_ntc2_raw,
                          (unsigned int)tBmsSnapshot.ntc.mos_raw,
                          (unsigned int)tBmsSnapshot.ntc.ptc_raw);
    }
    if (trigger == LOG_TRIGGER_SOFT_SHUTDOWN)
    {
        log->sw_err.bit.power_off_f = 1U; /* 仅在关机快照中置位，不修改BMS故障状态。 */
    }
    log->ex_sig = app_log_collect_ex_sig(&tBmsSnapshot);  /* 填入MOS和外部状态快照。 */
    /* 复位来源只在独立复位记录实际提交时填入。 */
    log->sys_clk = (uint8_t)(d_system_get_core_clock_hz() / 1000000UL); /* 保存当前主频，单位MHz。 */

    log->deg_data[0] = (uint8_t)trigger;        /* deg_data[0]保存故障、关机或独立复位记录的触发原因。 */
    log->deg_data[1] = s_log_overflow_count;    /* deg_data[1]保存队列溢出次数，用于判断是否丢过LOG。 */
    log->checksum = app_log_calc_checksum(log); /* 最后计算业务层校验值，填入checksum字段。 */
}

/* 把快照放入RAM队列；Flash写入留给SaveLog_Task的10ms周期处理。 */
static void app_log_push_pending(const log_rtd_t *log)
{
    if (s_log_pending_count >= APP_LOG_PENDING_NUM) /* 如果RAM队列已经满了。 */
    {
        if ((APP_LOG_PENDING_NUM > 1U) &&
            (s_log_pending[s_log_pending_tail].deg_data[0] == (uint8_t)LOG_TRIGGER_RESET))
        {
            uint8_t u8DropIndex = (uint8_t)((s_log_pending_tail + 1U) % APP_LOG_PENDING_NUM);
            uint8_t u8LastIndex = (uint8_t)((s_log_pending_head + APP_LOG_PENDING_NUM - 1U) % APP_LOG_PENDING_NUM);

            /* 队列满时保护最旧的独立复位记录，丢弃其后的最旧普通快照。 */
            while (u8DropIndex != u8LastIndex)
            {
                uint8_t u8NextIndex = (uint8_t)((u8DropIndex + 1U) % APP_LOG_PENDING_NUM);
                s_log_pending[u8DropIndex] = s_log_pending[u8NextIndex];
                u8DropIndex = u8NextIndex;
            }
            s_log_pending_head = u8LastIndex; /* 腾出的槽位作为新的head。 */
        }
        else
        {
            s_log_pending_tail = (uint8_t)((s_log_pending_tail + 1U) % APP_LOG_PENDING_NUM); /* 丢弃最旧一条普通快照。 */
        }
        s_log_pending_count--;       /* 因为丢了一条，所以队列数量先减1。 */
        s_log_overflow_count++;      /* 记录一次溢出，后续LOG中可以看到曾经丢过旧记录。 */
    }

    s_log_pending[s_log_pending_head] = *log; /* 把新快照复制到head位置。 */
    s_log_pending_head = (uint8_t)((s_log_pending_head + 1U) % APP_LOG_PENDING_NUM); /* head后移，形成环形队列。 */
    s_log_pending_count++; /* 队列有效数量加1。 */
}

/* 返回触发原因名称，方便通过RTT直接判断为什么保存这条快照。 */
static const char *app_log_trigger_name(uint8_t trigger)
{
    switch ((log_trigger_t)trigger)
    {
        case LOG_TRIGGER_FAULT_SET:
            return "fault-set";
        case LOG_TRIGGER_FAULT_CLEAR:
            return "fault-clear";
        case LOG_TRIGGER_FAULT_BOTH:
            return "fault-set-and-clear";
        case LOG_TRIGGER_SIGNAL_CHANGE:
            return "signal-change";
        case LOG_TRIGGER_RESET:
            return "reset";
        case LOG_TRIGGER_SOFT_SHUTDOWN:
            return "soft-shutdown";
        default:
            return "unknown";
    }
}

/* 一条快照成功写入Flash后，输出可读摘要和完整业务数据字节。 */
static void app_log_print_saved_record(const log_rtd_t *log)
{
    static const char s_hex_digits[] = "0123456789ABCDEF";
    const uint8_t *pu8LogBytes = (const uint8_t *)log;
    uint16_t u16ByteOffset;
    uint32_t u32CommitTickMs;

    if (log == NULL)
    {
        return;
    }

    u32CommitTickMs = d_timer_get_ms();

    if ((log->year == 0U) && (log->month == 0U) && (log->day == 0U))
    {
        SEGGER_RTT_printf(0,
                          "LOG_SAVED reason=%s(%u) event_time=uptime+%02u:%02u:%02u commit_tick=%lums\r\n",
                          app_log_trigger_name(log->deg_data[0]),
                          (unsigned int)log->deg_data[0],
                          (unsigned int)log->hour,
                          (unsigned int)log->min,
                          (unsigned int)log->sec,
                          (unsigned long)u32CommitTickMs);
    }
    else
    {
        SEGGER_RTT_printf(0,
                          "LOG_SAVED reason=%s(%u) event_time=20%02u-%02u-%02u %02u:%02u:%02u commit_tick=%lums\r\n",
                          app_log_trigger_name(log->deg_data[0]),
                          (unsigned int)log->deg_data[0],
                          (unsigned int)log->year,
                          (unsigned int)log->month,
                          (unsigned int)log->day,
                          (unsigned int)log->hour,
                          (unsigned int)log->min,
                          (unsigned int)log->sec,
                          (unsigned long)u32CommitTickMs);
    }

    SEGGER_RTT_printf(0,
                      "LOG_DATA soc=%u cell_min=%umV(#%u) cell_max=%umV(#%u) pack=%u*0.1V current=%d*10mA temp_cell_max=%dC temp_cell_min=%dC temp_mos=%d*0.1C\r\n",
                      (unsigned int)log->soc,
                      (unsigned int)log->minvol,
                      (unsigned int)log->min_cell_num,
                      (unsigned int)log->maxvol,
                      (unsigned int)log->max_cell_num,
                      (unsigned int)log->packvol,
                      (int)log->cur,
                      (int)log->max_cell_temp,
                      (int)log->min_cell_temp,
                      (int)log->mos_temp);
    SEGGER_RTT_printf(0,
                      "LOG_FLAGS hw=0x%04X warn=0x%08lX fault=0x%08lX signals=0x%08lX(detd=%u detc=%u) reset=0x%04X soc=0x%04X ports(chg=%u dsg=%u)*0.1V clk=%uMHz overflow=%u checksum=0x%02X\r\n",
                      (unsigned int)log->hw_err.status,
                      (unsigned long)log->sw_warn.status,
                      (unsigned long)log->sw_err.status,
                      (unsigned long)log->ex_sig.ex_sig,
                      (unsigned int)log->ex_sig.bit.detd_stu,
                      (unsigned int)log->ex_sig.bit.detc_stu,
                      (unsigned int)log->reset_s.reset_s,
                      (unsigned int)log->soc_s.soc_s,
                      (unsigned int)log->chg_port_vol,
                      (unsigned int)log->dsg_port_vol,
                      (unsigned int)log->sys_clk,
                      (unsigned int)log->deg_data[1],
                      (unsigned int)log->checksum);

    for (u16ByteOffset = 0U; u16ByteOffset < (uint16_t)sizeof(*log); u16ByteOffset += 16U)
    {
        char sHexLine[16U * 3U];
        uint16_t u16BytesInLine = (uint16_t)(sizeof(*log) - u16ByteOffset);
        uint16_t u16ByteIndex;
        uint16_t u16LineLength = 0U;

        if (u16BytesInLine > 16U)
        {
            u16BytesInLine = 16U;
        }

        for (u16ByteIndex = 0U; u16ByteIndex < u16BytesInLine; u16ByteIndex++)
        {
            uint8_t u8Value = pu8LogBytes[u16ByteOffset + u16ByteIndex];
            sHexLine[u16LineLength++] = s_hex_digits[u8Value >> 4U];
            sHexLine[u16LineLength++] = s_hex_digits[u8Value & 0x0FU];
            if ((u16ByteIndex + 1U) < u16BytesInLine)
            {
                sHexLine[u16LineLength++] = ' ';
            }
        }
        sHexLine[u16LineLength] = '\0';

        SEGGER_RTT_printf(0,
                          "LOG_RAW[%u..%u]=%s\r\n",
                          (unsigned int)u16ByteOffset,
                          (unsigned int)(u16ByteOffset + u16BytesInLine - 1U),
                          sHexLine);
    }
}

/* 每次只写一条快照到Flash，控制写Flash对主循环时基的影响。 */
static void app_log_write_one_pending(void)
{
    m_flash_wl_status_t eFlashAppendStatus; /* 保存本次Flash均衡擦写返回结果。 */

    if ((s_log_ready == 0U) || (s_log_pending_count == 0U)) /* Flash不可用或队列为空时不写。 */
    {
        return; /* 直接退出，避免无意义操作。 */
    }

    s_log_pending[s_log_pending_tail].reset_s =
        app_log_collect_reset_source(s_log_pending[s_log_pending_tail].deg_data[0]);
    s_log_pending[s_log_pending_tail].checksum =
        app_log_calc_checksum(&s_log_pending[s_log_pending_tail]);

    eFlashAppendStatus = m_flash_wl_append(&s_log_rtd_area,                         /* 指定写入实时故障快照均衡擦写区。 */
                               (const uint8_t *)&s_log_pending[s_log_pending_tail], /* 取队列中最旧的一条快照。 */
                               (uint16_t)sizeof(log_rtd_t));            /* 写入快照结构体的实际长度。 */
    if (eFlashAppendStatus == M_FLASH_WL_OK) /* 如果Flash写入成功。 */
    {
        if (s_log_pending[s_log_pending_tail].deg_data[0] == (uint8_t)LOG_TRIGGER_RESET)
        {
            s_log_reset_flags_pending = 0U; /* 只有独立复位记录成功提交才消费复位来源。 */
        }
        app_log_print_saved_record(&s_log_pending[s_log_pending_tail]); /* 只在记录确实提交到Flash后输出原因和内容。 */
        s_log_pending_tail = (uint8_t)((s_log_pending_tail + 1U) % APP_LOG_PENDING_NUM); /* tail后移，表示这条已消费。 */
        s_log_pending_count--; /* 队列有效数量减1。 */
    }
    else /* 如果Flash写入失败。 */
    {
        SEGGER_RTT_printf(0,
                          "LOG_WRITE_FAILED reason=%s(%u) status=%u\r\n",
                          app_log_trigger_name(s_log_pending[s_log_pending_tail].deg_data[0]),
                          (unsigned int)s_log_pending[s_log_pending_tail].deg_data[0],
                          (unsigned int)eFlashAppendStatus);
        s_log_ready = 0U; /* 关闭LOG写入，避免每秒反复尝试失败并占用任务时间。 */
    }
}

/* NTC原始故障掩码变化时输出通道和ADC值，区分传感器故障与单次采样毛刺。 */
static void app_log_check_ntc_diagnostics(void)
{
    app_bms_snapshot_t tBmsSnapshot;
    uint8_t bChanged;

    app_bms_snapshot_read(&tBmsSnapshot);
    bChanged = (uint8_t)((s_log_ntc_diag_inited == 0U) ||
                         (tBmsSnapshot.ntc.fault_mask != s_log_last_ntc_fault_mask) ||
                         (tBmsSnapshot.ntc_abnormal_count != s_log_last_ntc_count));
    if (bChanged == 0U)
    {
        return;
    }

    s_log_ntc_diag_inited = 1U;
    s_log_last_ntc_fault_mask = tBmsSnapshot.ntc.fault_mask;
    s_log_last_ntc_count = tBmsSnapshot.ntc_abnormal_count;

    SEGGER_RTT_printf(0,
                      "NTC_SAMPLE count=%u raw_fault[AFE=1,CELL1=2,CELL2=4,MOS=8,PTC=16]=0x%02X valid=0x%02X short=0x%02X open=0x%02X adc[cell1=%u cell2=%u mos=%u ptc=%u]\r\n",
                      (unsigned int)tBmsSnapshot.ntc_abnormal_count,
                      (unsigned int)tBmsSnapshot.ntc.fault_mask,
                      (unsigned int)tBmsSnapshot.ntc.valid_mask,
                      (unsigned int)tBmsSnapshot.ntc.short_mask,
                      (unsigned int)tBmsSnapshot.ntc.open_mask,
                      (unsigned int)tBmsSnapshot.ntc.cell_ntc1_raw,
                      (unsigned int)tBmsSnapshot.ntc.cell_ntc2_raw,
                      (unsigned int)tBmsSnapshot.ntc.mos_raw,
                      (unsigned int)tBmsSnapshot.ntc.ptc_raw);
}

/* 在启动最早阶段捕获复位标志，输出原始寄存器和逐项解码，便于定位复位来源。 */
void app_log_capture_reset_flags(void)
{
    d_system_get_reset_flags(&s_log_reset_flags);
    s_log_reset_flags_captured = 1U;
    s_log_reset_flags_pending = app_log_has_recordable_reset();

    SEGGER_RTT_printf(0,
                      "RESET raw=0x%08lX BOR=%u PIN=%u POR=%u SW=%u IWDG=%u WWDG=%u LPWR=%u\r\n",
                      (unsigned long)s_log_reset_flags.raw_flags,
                      (unsigned int)s_log_reset_flags.bor_reset,
                      (unsigned int)s_log_reset_flags.pin_reset,
                      (unsigned int)s_log_reset_flags.por_reset,
                      (unsigned int)s_log_reset_flags.soft_reset,
                      (unsigned int)s_log_reset_flags.iwdg_reset,
                      (unsigned int)s_log_reset_flags.wwdg_reset,
                      (unsigned int)s_log_reset_flags.lpwr_reset);
}

/* 初始化LOG管理模块：确保复位来源已捕获，扫描均衡擦写区，准备接收快照。 */
void app_log_init(void)
{
    m_flash_wl_status_t eFlashInitStatus; /* 保存Flash均衡擦写初始化结果。 */

    if (s_log_mutex == NULL)
    {
        s_log_mutex = xSemaphoreCreateRecursiveMutex();
    }

    if (s_log_reset_flags_captured == 0U)
    {
        app_log_capture_reset_flags(); /* 兼容未从main早期显式调用捕获接口的入口。 */
    }
    s_log_pending_head = 0U;          /* RAM队列写入位置从0开始。 */
    s_log_pending_tail = 0U;          /* RAM队列读取位置从0开始。 */
    s_log_pending_count = 0U;         /* 初始化时队列为空。 */
    s_log_overflow_count = 0U;        /* 初始化时还没有发生队列溢出。 */
    s_log_ntc_diag_inited = 0U;
    s_log_last_ntc_fault_mask = 0U;
    s_log_last_ntc_count = 0U;

    eFlashInitStatus = m_flash_wl_init(&s_log_rtd_area, &s_log_rtd_cfg); /* 扫描Flash LOG区，恢复写指针和最新记录位置。 */
    s_log_ready = ((eFlashInitStatus == M_FLASH_WL_OK) && (s_log_mutex != NULL)) ?
                  1U : 0U; /* Flash和队列互斥资源都可用才允许后续入队和写Flash。 */

    SEGGER_RTT_printf(0,
                      "LOG Flash region: [0x%08lX, 0x%08lX), size=%lu bytes, pages=%u, init=%s\r\n",
                      (unsigned long)APP_FLASH_MAP_LOG_RTD_START_ADDR,
                      (unsigned long)(APP_FLASH_MAP_LOG_RTD_START_ADDR +
                                      ((uint32_t)APP_LOG_RTD_PAGE_COUNT * APP_FLASH_MAP_FLASH_PAGE_SIZE)),
                      (unsigned long)((uint32_t)APP_LOG_RTD_PAGE_COUNT * APP_FLASH_MAP_FLASH_PAGE_SIZE),
                      (unsigned int)APP_LOG_RTD_PAGE_COUNT,
                      (s_log_ready != 0U) ? "ready" : "unavailable");

    /* 非纯上电复位必须生成独立记录，不能附着在后续故障快照上。 */
    if ((s_log_ready != 0U) && (s_log_reset_flags_pending != 0U))
    {
        app_log_request_snapshot(LOG_TRIGGER_RESET);
    }
}

/* 10ms任务入口：输出采样诊断，再最多写一条等待中的快照。 */
void app_log_task(void)
{
    if (!app_log_lock(portMAX_DELAY))
    {
        return;
    }

    if (s_log_ready == 0U) /* 如果LOG Flash区不可用。 */
    {
        app_log_unlock();
        return; /* 直接退出，不做状态检查和写Flash。 */
    }

    app_log_check_ntc_diagnostics(); /* 输出NTC原始故障掩码变化，便于辨认异常通道。 */
    app_log_write_one_pending();   /* 每10ms最多写一条RAM队列中的快照到Flash。 */
    app_log_unlock();
}

/* 故障模块调用这个接口请求保存快照；这里只入队，不在故障任务里写Flash。 */
bool app_log_request_snapshot(log_trigger_t trigger)
{
    log_rtd_t log; /* 临时快照变量，先在栈上组装完整LOG。 */

    /* 保留旧原因编号用于历史日志解码，但不再接受MOS/外部信号变化请求。 */
    if (trigger == LOG_TRIGGER_SIGNAL_CHANGE)
    {
        return false;
    }

    if (!app_log_lock(pdMS_TO_TICKS(100U)))
    {
        return false;
    }

    if (s_log_ready == 0U)
    {
        app_log_unlock();
        return false;
    }

    app_log_build_snapshot(&log, trigger); /* 根据当前系统状态生成一条快照。 */
    app_log_push_pending(&log);            /* 把快照放入RAM队列，等待10ms任务写Flash。 */
    app_log_unlock();
    return true;
}

app_log_flush_status_t app_log_get_flush_status(void)
{
    app_log_flush_status_t status;

    /* 此接口在BMS实时任务里调用，不等待正在进行的Flash写操作。 */
    if (!app_log_lock(0U))
    {
        return (s_log_ready == 0U) ? APP_LOG_FLUSH_FAILED : APP_LOG_FLUSH_PENDING;
    }

    if (s_log_ready == 0U)
    {
        status = APP_LOG_FLUSH_FAILED;
    }
    else
    {
        status = (s_log_pending_count == 0U) ? APP_LOG_FLUSH_COMPLETE : APP_LOG_FLUSH_PENDING;
    }

    app_log_unlock();
    return status;
}

/* 返回当前已经写入Flash并通过底层CRC校验的实时LOG条数。 */
uint16_t app_log_rtd_num(void)
{
    m_flash_wl_info_t tFlashAreaInfo; /* 临时保存底层均衡擦写分区状态。 */

    if (s_log_ready == 0U) /* LOG分区初始化失败时不能导出。 */
    {
        return 0U; /* 对外显示没有可读记录。 */
    }

    if (m_flash_wl_get_info(&s_log_rtd_area, &tFlashAreaInfo) != M_FLASH_WL_OK) /* 读取底层状态失败。 */
    {
        return 0U; /* 对外显示没有可读记录。 */
    }

    if (tFlashAreaInfo.valid_record_count > 0xFFFFUL) /* 通信协议里条数用uint16_t。 */
    {
        return 0xFFFFU; /* 理论上当前48K区达不到，这里做饱和保护。 */
    }

    return (uint16_t)tFlashAreaInfo.valid_record_count; /* 返回有效记录总数。 */
}

/* 按通信协议索引读取实时LOG，index从1开始。 */
bool app_log_rtd_read(log_rtd_t *buffer, uint16_t index)
{
    uint16_t u16ReadLength; /* 底层读出的业务数据长度。 */

    if ((s_log_ready == 0U) || (buffer == NULL)) /* LOG不可用或输出指针为空。 */
    {
        return false; /* 读取失败。 */
    }

    if (m_flash_wl_read_by_index(&s_log_rtd_area, index,
                                 (uint8_t *)buffer, (uint16_t)sizeof(*buffer),
                                 &u16ReadLength, NULL) != M_FLASH_WL_OK) /* 让底层按索引读取。 */
    {
        return false; /* 底层未找到或CRC失败。 */
    }

    return (u16ReadLength == (uint16_t)sizeof(*buffer)); /* 长度必须正好等于当前LOG业务结构体长度。 */
}

/* 复制LOG Flash分区和RAM待写队列状态，便于判断记录是否丢失或写入失败。 */

