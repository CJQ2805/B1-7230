#ifndef __APP_PARAM_H__
#define __APP_PARAM_H__

/*
 * 电池/平台型号：
 * 主要用于升级文件命名和项目区分，不直接参与保护逻辑。
 */
 /* 厂家名称，看客户需求而改变，默认为 DUCAR */
#define APP_PARAM_MANUFACTURER_NAME "DUCAR"

/* 电池/平台型号，用于升级文件命名、通信识别和项目区分。 */
#define APP_PARAM_BAT_TYPE          "7230B1"


/* 额定电压   mV*/
#define APP_PARAM_BAT_VOLTAGE          72000   //72V

/* 额定容量   mAh*/
#define APP_PARAM_BAT_CAPACITY         30000   //30Ah
/* 最大允许充电电压，单位mV；当前20S电池包配置为84V。 */
#define APP_PARAM_MAX_CHARGE_VOLTAGE_MV 84000U

/* 宗申B1主动通信：KEY缺失时，电流归零达到该时间后停止发送，单位ms。 */
#define APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS (1UL * 60UL * 1000UL)
/* UDS诊断总开关：0表示不初始化、不收发且不运行UDS；1表示启用安全诊断服务。 */
#define APP_PARAM_UDS_ENABLE           1U

/*
 * 硬件版本：
 * 上位机读取硬件版本时回复该字段，用来区分PCB版本或硬件配置差异。
 */
/* 当前硬件版本字符串，用于上位机读取和硬件版本识别。 */
#define APP_PARAM_HW_VERSION        "V01"
/*
 * 出包V版本：
 * 打包文件名使用，显示时带V前缀，例如V001。
 */
/* 升级包V版本字符串，用于生成升级文件名，用于产线生产版本，每次大生产必须+1，在生产中只迭代小版本。 */
#define APP_PARAM_PACKAGE_V_VERSION "V01"
/*
 * 出包T版本：
 * 打包文件名使用，显示时带T前缀，用于临时调试版本，不进行生产只迭代小版本，生产时大版本V+1的情况下，小版本必须置1，例如T1,最小单位为1。 */
/* 升级包T版本字符串，用于生成升级文件名。 */
#define APP_PARAM_PACKAGE_T_VERSION "T14"

/* DUCAR CAN通信协议版本：A0.9=0x09，F200的Data3返回此标识。 */
#define APP_PARAM_CAN_PROTOCOL_VERSION 0x0AU

/*
 * BMS硬件拓扑配置：
 *   这些值会随电池串数、AFE型号和外部NTC接线变化，属于项目配置，不属于数据类型定义。
 *   快照、故障、通信、LOG和低功耗通过app_bms_types.h间接使用这些统一配置。
 */
/* AFE原始采样/物理拓扑的最大容量；DVC1124最多支持24个物理单体通道。 */
#define APP_BMS_MAX_CELL_NUM       24U
/* 当前电池包逻辑串数；快照cell_mv数组按此长度分配，更换20S/21S方案时调整此项。 */
#define APP_BMS_CELL_COUNT         20U
/* 当前板级接线要求完整读取DVC1124的24个物理单体通道。 */
#define APP_BMS_AFE_SCAN_CELL_COUNT 24U
/* 物理顶节固定接在AFE第24通道，数组下标为23。 */
#define APP_BMS_AFE_TOP_CELL_INDEX 23U

/* AFE侧参与电芯温度统计的NTC数量。 */
#define AFE_NTC_NUM                1U
/* MCU ADC侧参与电芯温度统计的NTC数量。 */
#define MCU_NTC_NUM                2U



/*
 * 控制器欠压保护点
 * 正常应该分成低温和高温两个值，  cell_mv * cell_num
 */
#define CONTROLLER_UNDER_VOLTAGE_PROTECT            		3000*APP_BMS_CELL_COUNT              //mV
/* F220最低有效电芯温度≤-1.0℃时发送该值；温度≥+1.0℃后恢复常温值。 */
#define CONTROLLER_UNDER_VOLTAGE_PROTECT_MINUS_0_TMEP       3000*APP_BMS_CELL_COUNT               //mV
/*
 * 均衡策略配置：
 *   均衡开关、启动/停止压差和允许均衡的最低单体电压都会随电芯方案调整，
 *   因此统一放在项目参数文件中，均衡算法文件只保留判断和执行逻辑。
 */
/* 均衡功能总开关：1表示允许均衡逻辑运行，0表示关闭均衡。 */
#define APP_BALANCE_ENABLE              1U
#if (APP_BALANCE_ENABLE != 0U)
/* 均衡启动压差阈值，单位mV；单体最大压差达到该值才允许启动均衡。 */
#define APP_BALANCE_START_DIFF_MV       30U
/* 均衡停止压差阈值，单位mV；压差降到该值及以下时停止均衡。 */
#define APP_BALANCE_STOP_DIFF_MV        10U
/* 允许均衡的最低单体电压，单位mV；最低单体电压必须高于该值。 */
#define APP_BALANCE_MIN_CELL_MV         3200U
#endif 

/*
 * AFE项目参数：
 *   下面的值描述当前项目实际使用的DVC1124采样拓扑和采样电阻，不是寄存器定义。
 *   逻辑电池串数修改APP_BMS_CELL_COUNT；AFE物理扫描范围和末节接线通道分别修改对应参数；
 *   分步读取数量属于驱动时序常量，固定保留在hsi_afe.c中。
 */
/* AFE电流采样电阻，单位mΩ；用于将DVC1124电流寄存器值换算为mA。 */
#define APP_AFE_CURRENT_SENSE_RESISTANCE_MR 0.2f

/*
 * APP参数集中管理。
 *
 * 这里放“整机项目级参数”，也就是不同客户、不同品号、不同出包版本会调整的内容。
 * 这些参数不属于某一个故障、不属于某一个驱动，所以不要散落在通信、保护或打包工具里。
 *
 * 修改建议：
 * 1. 品号、版本号类参数在立项、出样、量产改版时修改。
 * 2. 电流零点死区这类整机策略参数也放在这里，保证通信、LOG、保护、低功耗看到同一份处理结果。
 * 3. 这里的字符串宏会被通信回复和打包工具共同使用，改动后需要重新编译APP并重新生成升级文件。
 */

/*
 * 软件品号：
 * 固定10字节ASCII字符串，上位机升级匹配和通信读取都会使用。
 * A000000000是默认强制升级品号，用于未定义正式品号或工装强制升级场景。
 */
/* 软件品号，固定10字节ASCII；用于升级匹配、通信读取和升级文件命名。 */
#define APP_PARAM_SOFTWARE_PN       "A800000002"

/*
 * 成品条码/软件序列号：
 * 最多预留50字节ASCII，供协议0x17读取和协议0x62写入。
 *
 * 当前工程只有成品条码，没有半成品条码，因此不再增加half_code字段。
 * 该宏仅作为Flash中没有有效SN记录时的出厂默认值；协议0x62写入成功后，运行时
 * 读取值以Flash保存的SN为准。
 * SN内容不决定生产/出货模式；模式由CAN 0x0800的Data1独立设置。
 */
/* Flash中没有有效SN时使用的默认制造SN；空字符串表示SN未写入。 */
#define APP_BMS_MANUFACTURE_SN      ""


/*
 * 电流零点死区，单位mA。
 *
 * AFE底层换算出真实电流后，会先经过这个死区处理：
 * - 电流绝对值小于该值时，统一归零。
 * - 归零后的业务电流给保护、方向判断和低功耗使用；
 * - current_ma仍保留校准后的实际小电流，供显示、通信和LOG使用。
 *
 * 当前配置为300mA：
 * - 299mA以内认为是采样零漂或无明显电流，按0mA处理。
 * - 大于等于300mA才认为确实存在充电或放电电流。
 */
/* 电流零点死区数值，单位mA。 */
#define APP_PARAM_CURRENT_ZERO_DEADBAND_MA  300

/* 当前项目CAN1总线波特率，单位kbit/s；整车节点必须使用相同配置。 */
#define APP_PARAM_CAN1_BAUDRATE_KBPS         500U

/*
 * 低功耗策略配置：
 *   这些参数决定APP多久进入STOP、RTC多久巡检一次以及串口空闲判定相关策略。
 *   时钟切换超时属于低功耗流程固定保护常量，保留在app_low_power_manage.c中。
 */
/* 连续满足低功耗条件后进入STOP的等待时间，单位秒。 */
#define APP_LOW_POWER_IDLE_SECONDS        60U
/* STOP期间RTC主动唤醒巡检周期，单位秒。最大不能超过25S，否则会看门狗复位 */
#define APP_LOW_POWER_RTC_WAKE_SECONDS    20U
/* SOC单体OCV查表数据：索引0~100对应0%~100% SOC，表值单位mV。 */
#define APP_PARAM_SOC_CELL_OCV_TABLE \
    { \
        2801U, 3010U, 3132U, 3191U, 3216U, 3231U, 3244U, 3259U, 3276U, 3296U, \
        3318U, 3343U, 3370U, 3396U, 3419U, 3439U, 3455U, 3469U, 3481U, 3491U, \
        3500U, 3509U, 3517U, 3524U, 3531U, 3537U, 3544U, 3550U, 3555U, 3561U, \
        3566U, 3572U, 3577U, 3582U, 3588U, 3593U, 3598U, 3604U, 3610U, 3615U, \
        3621U, 3627U, 3634U, 3641U, 3648U, 3656U, 3664U, 3672U, 3681U, 3690U, \
        3699U, 3709U, 3718U, 3728U, 3738U, 3747U, 3757U, 3766U, 3774U, 3782U, \
        3790U, 3797U, 3804U, 3811U, 3816U, 3822U, 3827U, 3832U, 3836U, 3840U, \
        3844U, 3848U, 3852U, 3857U, 3863U, 3871U, 3880U, 3892U, 3905U, 3919U, \
        3934U, 3949U, 3964U, 3979U, 3994U, 4010U, 4025U, 4041U, 4056U, 4071U, \
        4086U, 4100U, 4113U, 4126U, 4139U, 4152U, 4164U, 4177U, 4191U, 4208U, \
        4280U \
    }

#endif
