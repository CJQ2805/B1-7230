#ifndef _APP_LOG_MANAGE_H_
#define _APP_LOG_MANAGE_H_

#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_APP_Log    1
#define Module_Minor_Version_APP_Log    2

#define LOG_CHGDSG_NUM                  5U
#define LOG_RTD_PAYLOAD_SIZE            70U  /* 实时快照业务结构体固定70字节。 */
#define LOG_RTD_STORE_SIZE              92U  /* 底层槽位：16字节记录头 + 70字节业务数据 + 4字节提交标志，再按4字节对齐。 */

/* 快照触发来源，写入deg_data[0]，后续导出LOG时可以知道这条记录为什么保存。 */
typedef enum
{
    LOG_TRIGGER_FAULT_SET = 1,      /* 故障位从0变1。 */
    LOG_TRIGGER_FAULT_CLEAR,        /* 故障位从1变0。 */
    LOG_TRIGGER_FAULT_BOTH,         /* 同一轮同时存在故障置位和清除。 */
    LOG_TRIGGER_SIGNAL_CHANGE,      /* 外部状态或MOS状态变化。 */
    LOG_TRIGGER_RESET,              /* 上电检测到独立看门狗复位。 */
    LOG_TRIGGER_SOFT_SHUTDOWN = 6   /* 用户或协议请求软关机。 */
} log_trigger_t;

/* 实时LOG队列的持久化状态，供关机流程有界等待最终快照。 */
typedef enum
{
    APP_LOG_FLUSH_COMPLETE = 0U,
    APP_LOG_FLUSH_PENDING,
    APP_LOG_FLUSH_FAILED
} app_log_flush_status_t;

/* LOG记录类型，底层均衡擦写只负责存字节，上层用这个区分数据含义。 */
typedef enum
{
    LOG_RECORD_RTD = 1,          /* BMS实时故障快照。 */
    LOG_RECORD_DSG_HEAD,         /* 放电开始记录。 */
    LOG_RECORD_DSG_REAR,         /* 放电结束记录。 */
    LOG_RECORD_CHG_HEAD,         /* 充电开始记录。 */
    LOG_RECORD_CHG_REAR          /* 充电结束记录。 */
} log_record_type_t;

#pragma pack(1)

/* AFE硬件故障：来自DVC1124硬件状态寄存器或ALERT故障源。 */
typedef union
{
    uint16_t status;
    struct
    {
        uint16_t sc     : 1; /* 放电短路。 */
        uint16_t dsgoc2 : 1; /* 二级放电过流。 */
        uint16_t dsgoc1 : 1; /* 一级放电过流。 */
        uint16_t chgut  : 1; /* 充电低温。 */
        uint16_t chgot  : 1; /* 充电高温。 */
        uint16_t dsgot  : 1; /* 放电高温。 */
        uint16_t celluv : 1; /* 单体欠压。 */
        uint16_t cellov : 1; /* 单体过压。 */
        uint16_t chgoc  : 1; /* 充电过流。 */
        uint16_t r2     : 3;
        uint16_t dsgut  : 1; /* 放电低温。 */
        uint16_t r3     : 3;
    } bit;
} log_hw_err_t;

/* 软件告警：只保存Warning类轻故障，不能和Protection/Lock故障合在一起。 */
typedef union
{
    uint32_t status;
    struct
    {
        uint32_t dsgoc1        : 1; /* 统一放电过流告警。 */
        uint32_t dsgoc2        : 1; /* 兼容旧记录的预留告警位，不再表示独立等级。 */
        uint32_t dsgot         : 1; /* 放电高温告警。 */
        uint32_t dsgut         : 1; /* 放电低温告警。 */
        uint32_t sc            : 1; /* 短路告警预留，当前没有告警源。 */
        uint32_t celluv        : 1; /* 单体或总压欠压告警。 */
        uint32_t celluv2       : 1; /* 二级欠压告警预留，当前没有告警源。 */
        uint32_t chgoc         : 1; /* 充电过流或反向电流告警。 */
        uint32_t chgot         : 1; /* 充电高温告警。 */
        uint32_t chgut         : 1; /* 充电低温告警。 */
        uint32_t cellov1       : 1; /* 单体或总压过压告警。 */
        uint32_t cellov2       : 1; /* 二级过压告警预留，当前没有告警源。 */
        uint32_t ovpf          : 1; /* 过压永久故障告警预留。 */
        uint32_t chgmosfail    : 1; /* 充电MOS失效告警预留。 */
        uint32_t dsgmosfail    : 1; /* 放电MOS失效告警预留。 */
        uint32_t badcell       : 1; /* 单体异常告警预留。 */
        uint32_t unbalance     : 1; /* 单体压差告警。 */
        uint32_t ntc_warn      : 1; /* 仅一路NTC电气异常，属于NTC-Warning，不执行保护。 */
        uint32_t r4            : 1; /* 保留位。 */
        uint32_t mos_ot        : 1; /* MOS过温告警。 */
        uint32_t aps_dsgoc     : 1; /* 辅助电源放电过流告警预留。 */
        uint32_t aps_sc        : 1; /* 辅助电源短路告警预留。 */
        uint32_t sys_clk_err   : 1; /* 系统时钟告警预留。 */
        uint32_t auth_chg_fail : 1; /* 充电鉴权告警预留。 */
        uint32_t power_off_f   : 1; /* 关机告警预留。 */
        uint32_t fuse_on_f     : 1; /* 熔断告警预留。 */
        uint32_t afe_err_f     : 1; /* AFE告警预留。 */
        uint32_t hardfault_f   : 1; /* MCU异常告警预留。 */
        uint32_t r5            : 4;
    } bit;
} log_sw_warn_t;

/* 软件故障：只保存pro/fault/lock/fuse类状态，告警已经单独放到log_sw_warn_t。 */
typedef union
{
    uint32_t status;
    struct
    {
        uint32_t dsgoc1        : 1; /* 一级放电过流保护。 */
        uint32_t dsgoc2        : 1; /* 二级及以上放电过流保护或锁定。 */
        uint32_t dsgot         : 1; /* 放电高温。 */
        uint32_t dsgut         : 1; /* 放电低温。 */
        uint32_t sc            : 1; /* 短路。 */
        uint32_t celluv        : 1; /* 单体欠压。 */
        uint32_t celluv2       : 1; /* 二级单体欠压。 */
        uint32_t chgoc         : 1; /* 充电过流。 */
        uint32_t chgot         : 1; /* 充电高温。 */
        uint32_t chgut         : 1; /* 充电低温。 */
        uint32_t cellov1       : 1; /* 一级单体过压。 */
        uint32_t cellov2       : 1; /* 二级单体过压。 */
        uint32_t ovpf          : 1; /* 过压永久故障。 */
        uint32_t chgmosfail    : 1; /* 充电MOS失效。 */
        uint32_t dsgmosfail    : 1; /* 放电MOS失效。 */
        uint32_t badcell       : 1; /* 单体异常。 */
        uint32_t unbalance     : 1; /* 单体压差/不均衡。 */
        uint32_t chg_pro       : 1; /* 充电保护汇总，协议软件故障Bit17。 */
        uint32_t dsg_pro       : 1; /* 放电保护汇总，协议软件故障Bit18。 */
        uint32_t mos_ot        : 1; /* MOS过温。 */
        uint32_t aps_dsgoc     : 1; /* 辅助电源放电过流。 */
        uint32_t aps_sc        : 1; /* 辅助电源短路。 */
        uint32_t sys_clk_err   : 1; /* 系统时钟异常。 */
        uint32_t auth_chg_fail : 1; /* 充电鉴权失败。 */
        uint32_t power_off_f   : 1; /* 关机事件。 */
        uint32_t fuse_on_f     : 1; /* 熔断触发事件。 */
        uint32_t afe_err_f     : 1; /* AFE通信/数据异常。 */
        uint32_t hardfault_f   : 1; /* MCU发生HardFault。 */
        uint32_t ntc_fault     : 1; /* 至少两路NTC电气异常，协议软件故障Bit28。 */
        uint32_t r5            : 3;
    } bit;
} log_sw_err_t;

/* 外部输入和MOS输出状态快照。 */
typedef union
{
    uint32_t ex_sig;
    struct
    {
        uint32_t detd_stu      : 1; /* 放电检测信号。 */
        uint32_t detc_stu      : 1; /* 充电检测信号。 */
        uint32_t auth_dsg_stu  : 1; /* 放电鉴权状态。 */
        uint32_t acc1_stu      : 1; /* ACC1状态。 */
        uint32_t acc2_stu      : 1; /* ACC2状态。 */
        uint32_t dcsc_stu      : 1; /* 次回路短路信号。 */
        uint32_t dcdet_stu     : 1; /* DCDC接入信号。 */
        uint32_t chg_in_stu    : 1; /* 充电器接入信号。 */
        uint32_t id1_stu       : 1; /* ID1状态。 */
        uint32_t id2_stu       : 1; /* ID2状态。 */
        uint32_t bmr_stu       : 1; /* BMR状态。 */
        uint32_t heat_stu      : 1; /* 加热状态。 */
        uint32_t chg_mos_s     : 1; /* 充电MOS命令/状态。 */
        uint32_t dsg_mos_s     : 1; /* 放电MOS命令/状态。 */
        uint32_t chgd_mos_s    : 1; /* 充放电MOS组合状态。 */
        uint32_t pre_mos_s     : 1; /* 预放MOS状态。 */
        uint32_t r2            : 8;
        uint32_t r3            : 8;
    } bit;
} log_ex_sig_t;

/* 复位来源，后面由RCC->CTRLSTS转换填充。 */
typedef union
{
    uint16_t reset_s;
    struct
    {
        uint16_t v18pwr_rstf : 1; /* 欠压复位。 */
        uint16_t obl_rst     : 1; /* 选项字节加载复位。 */
        uint16_t pin_rst     : 1; /* NRST引脚复位。 */
        uint16_t por_rst     : 1; /* 上电/掉电复位。 */
        uint16_t sft_rst     : 1; /* 软件复位。 */
        uint16_t iwd_rst     : 1; /* 独立看门狗复位。 */
        uint16_t wwd_rst     : 1; /* 窗口看门狗复位。 */
        uint16_t lpwr_rst    : 1; /* 低功耗复位。 */
        uint16_t boot_rst    : 1; /* Boot或升级流程触发复位。 */
        uint16_t r1          : 7;
    } bit;
} log_reset_s_t;

/* SOC校准和SOC相关事件。 */
typedef union
{
    uint16_t soc_s;
    struct
    {
        uint16_t dsg_full_flag         : 1; /* 放电满电修正标志。 */
        uint16_t dsg_vbat_ecu_p_flag  : 1; /* 放电总压/ECU条件修正标志。 */
        uint16_t min_voltage_lt_2v9   : 1; /* 最低单体低于2.9V。 */
        uint16_t dsg_vbat_soc6_flag   : 1; /* 放电总压SOC6条件修正。 */
        uint16_t idle_soc_cali_flag   : 1; /* 静置SOC校准。 */
        uint16_t r1                   : 3;
        uint16_t r2                   : 8;
    } bit;
} log_soc_s_t;

/* BMS实时快照，一条业务记录固定70字节，底层存储时按92字节记录槽保存。 */
typedef struct
{
    uint8_t year;              /* 年，建议保存为2000年后的偏移。 */
    uint8_t month;             /* 月。 */
    uint8_t day;               /* 日。 */
    uint8_t hour;              /* 时。 */
    uint8_t min;               /* 分。 */
    uint8_t sec;               /* 秒。 */
    uint8_t soc;               /* SOC，单位：%。 */
    uint16_t maxvol;           /* 最高单体电压，单位：mV。 */
    uint16_t minvol;           /* 最低单体电压，单位：mV。 */
    int16_t max_cell_temp;     /* 最高电芯温度，单位摄氏度，来自统一业务快照。 */
    int16_t cur;               /* 主回路实际电流，单位10mA，保留校准后小电流。 */
    log_hw_err_t hw_err;       /* AFE硬件故障。 */
    log_sw_warn_t sw_warn;     /* 软件告警，只保存Warning类状态。 */
    log_sw_err_t sw_err;       /* 软件故障，只保存pro/fault/lock/fuse类状态。 */
    uint8_t reserved_current[4]; /* 原充电支路/辅助电源电流位置：当前没有独立采样源，固定预留。 */
    uint8_t max_cell_num;      /* 最高单体编号。 */
    uint8_t min_cell_num;      /* 最低单体编号。 */
    uint16_t packvol;          /* 电池包总压，单位：0.1V。 */
    uint8_t max_temp_num;      /* 最高温度编号。 */
    uint8_t min_temp_num;      /* 最低温度编号。 */
    int16_t min_cell_temp;     /* 最低电芯温度，单位摄氏度，来自统一业务快照。 */
    int16_t mos_temp;          /* MOS温度，单位0.1摄氏度；无效时保留0xFFFF。 */
    log_ex_sig_t ex_sig;       /* 外部信号和MOS状态。 */
    log_reset_s_t reset_s;     /* 复位来源。 */
    log_soc_s_t soc_s;         /* SOC校准状态。 */
    uint16_t chg_port_vol;     /* 充电口电压，单位：0.1V。 */
    uint16_t dsg_port_vol;     /* 放电口电压，单位：0.1V。 */
    uint8_t sys_clk;           /* 当前系统主频，单位：MHz。 */
    uint8_t deg_data[4];       /* 降级/通信/扩展诊断数据。 */
    uint8_t reserved_store[13]; /* 业务结构体扩展预留，使快照固定为70字节。 */
    uint8_t checksum;          /* 业务层简单校验，底层另有CRC。 */
} log_rtd_t;

typedef char log_rtd_size_check[(sizeof(log_rtd_t) == LOG_RTD_PAYLOAD_SIZE) ? 1 : -1];

/* 边沿检测辅助对象，用于故障变化时触发保存。 */
typedef struct
{
    uint64_t monit_s;
    uint64_t monit_last_s;
} log_monit_s_t;

typedef enum
{
    LOG_Both = 0,
    LOG_Rising,
    LOG_Falling
} log_sig_edge_t;

typedef union
{
    uint16_t date;
    struct
    {
        uint16_t day   : 5; /* 1-31。 */
        uint16_t month : 4; /* 1-12。 */
        uint16_t year  : 7; /* 以1980年为基准。 */
    } bit;
} log_date_t;

/* 充放电开始/结束时的单体电压摘要。 */
typedef struct
{
    log_date_t date;
    uint16_t maxvol;
    uint16_t minvol;
    uint8_t maxvol_num;
    uint8_t minvol_num;
} log_dsgchg_t;

#pragma pack()

/* 初始化LOG均衡擦写区域；只扫描已有记录，不会主动格式化整区。 */
void app_log_init(void);
/* 在main最早阶段读取并输出硬件复位原因，防止后续初始化或复位清除RCC标志。 */
void app_log_capture_reset_flags(void);
/* LOG周期处理入口：SaveLog_Task每10ms调用一次，检查状态变化，并最多写入一条等待中的快照。 */
void app_log_task(void);
/* 请求保存一次实时快照；成功返回true，失败时不入队；只入队、不在调用现场写Flash。 */
bool app_log_request_snapshot(log_trigger_t trigger);
/* 返回当前实时LOG队列是否已写完、仍在等待，或存储不可用。 */
app_log_flush_status_t app_log_get_flush_status(void);
/* 读取当前Flash中有效实时LOG条数；返回值按通信协议限制为uint16_t。 */
uint16_t app_log_rtd_num(void);
/* 按1起始索引读取实时LOG；index=1表示最旧记录，index=app_log_rtd_num()表示最新记录。 */
bool app_log_rtd_read(log_rtd_t *buffer, uint16_t index);

#endif
