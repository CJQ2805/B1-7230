#ifndef _HSI_AFE_H_
#define _HSI_AFE_H_

#include <stdint.h>
#include <stdbool.h>
#include "App/app_param.h"
#include "Middle/m_delay.h"
#include "Middle/m_soft_i2c.h"
#include "hsi_afe_dvc1124.h"

#define HSI_AFE_MUTEX_TIMEOUT_MS  ((uint32_t)20UL)

/* DVC1124 R0故障掩码，用于清除或解析AFE硬件故障标志。 */
#define	AFE_MASK_SCD	0x01	//放电短路
#define	AFE_MASK_OCC2	0x02	//2级充电过流
#define	AFE_MASK_OCD2	0x04	//2级放电过流
#define	AFE_MASK_OCC1	0x08	//1级充电过流
#define	AFE_MASK_OCD1	0x10	//1级放电过流
#define	AFE_MASK_CUV	0x20	//电池欠压
#define	AFE_MASK_COV	0x40	//电池过压
#define	AFE_MASK_IWF	0x80	//IIC看门狗超时
#define	AFE_MASK_NA		0x00	//无效标志

/* 均衡掩码按DVC1124寄存器字节分组，bit0对应物理通道1，容量覆盖全部AFE通道。 */
#define HSI_AFE_BALANCE_MASK_ALL ((1UL << APP_BMS_MAX_CELL_NUM) - 1UL)
typedef enum
{
BALANCE_MASK_L = (HSI_AFE_BALANCE_MASK_ALL & 0x0000FFUL),
BALANCE_MASK_M = (HSI_AFE_BALANCE_MASK_ALL & 0x00FF00UL),
BALANCE_MASK_H = (HSI_AFE_BALANCE_MASK_ALL & 0xFF0000UL),
BALANCE_MASK_All = HSI_AFE_BALANCE_MASK_ALL
}Blance_Mask;

/* MOS_IG表示忽略/无命令，MOS_OFF和MOS_ON是真实控制命令。 */
typedef enum
{
	MOS_IG = -1,
	MOS_OFF = 0,
	MOS_ON
}MOS_State;

/* AFE状态缓存，供故障处理和MOS控制使用。 */
typedef union
{
struct
{
	uint8_t chgov : 1;      //过压保护
	uint8_t dsguv : 1;      //欠压保护
	uint8_t dsgoc : 1;      //放电过流保护
	uint8_t chgoc : 1;      //充电过流保护
	uint8_t sc : 1;         //短路保护
	uint8_t dsgoc2 : 1;      //放电二级过流保护
	uint8_t chgoc2 : 1;      //充电二级过流保护
	uint8_t rev1 : 1;         //短路保护

	uint8_t chgmos: 1;		//充电驱动状态
	uint8_t dsgmos: 1;		//充电驱动状态
	uint8_t iicwdgerr:1;   	//IIC看门狗溢出
	uint8_t rev2 : 5;         //短路保护

	uint8_t chipsta:4;  	//芯片当前工作状态
	uint8_t cc2finish:1; 	//CC2采样完成
	uint8_t cc1finish:1; 	//CC1采样完成
	uint8_t volfinish:1; 	//电压采样转换完成
	uint8_t chgdet:1;   	//充电器检测
}bits;
uint8_t data[3];

}hsi_afe_status_t;

/* DVC1124供电、硬关闭、告警相关板级IO；空IO表示未使用。 */
typedef  struct
{
	d_io_t vdd5;    	//供电
	d_io_t chg_ctrl;    //充电关断
	d_io_t dsg_ctl;    	//放电关断
	d_io_t alert;
}hsi_afe_dvc1124_ios_t;

/* AFE运行对象：包含IO、IIC总线、分步读取状态、测量值和状态缓存。 */
typedef struct
{
	hsi_afe_dvc1124_ios_t   ios;
	m_soft_i2c_t*            i2c;

	uint8_t             task;           /* 分步读取任务序号。 */
	volatile uint8_t    alert_pending;  /* ALERT外部中断置位，hsi_afe_service()中消费。 */
	volatile uint8_t    shutdown_requested; /* SHUTDOWN命令已发出，AFE任务不得再访问芯片。 */
	uint8_t             cells;          /* 业务逻辑串数，例如当前20S；快照按此数量输出。 */
	uint8_t             cell_scan_count;/* AFE物理采样通道数；当前按接线完整扫描24路。 */
	uint32_t            cell_ready_mask;/* AFE物理通道分步读取完成标志，每一位对应一路。 */
	uint8_t             data_ready;     /* 1表示配置的全部AFE物理扫描通道已成功刷新一轮。 */
	uint16_t            vol[APP_BMS_MAX_CELL_NUM];
	int32_t             curr_reg;   //前端电流寄存器值，非电流值
	int32_t             curr_raw;    //AFE换算后的原始电流值，单位mA，不做零死区处理
	int32_t             curr_calibrated; //经过K/B校准的电流值，单位mA，不做零死区处理
	int32_t             curr_real;   //业务使用电流值，单位mA，经过K/B校准和零死区处理
	uint32_t            current_update_seq; /* 电流成功刷新序号，供校准模块识别新样本。 */
	int16_t             temp[AFE_NTC_NUM];
	hsi_afe_dvc1124_ntc_status_t ntc_status; /* AFE_NTC最近一次采样状态，不把IIC失败当作NTC电气故障。 */
	hsi_afe_status_t    status;
	uint8_t             status_valid;   /* 最近一次R0/R1/R119状态寄存器读取是否完整成功。 */
	uint32_t            vpack;
	uint32_t            vbat;
	uint32_t            balstate;

}hsi_afe_t;

/* 初始化并启用AFE硬件/数据资源守卫，必须在创建业务任务前调用。 */
bool hsi_afe_resource_init(void);
/* 获取AFE硬件/数据资源；支持同一任务递归获取，超时返回false。 */
bool hsi_afe_acquire(uint32_t timeout_ms);
/* 释放AFE硬件/数据资源。 */
void hsi_afe_release(void);

/* 初始化DVC1124 IO、芯片寄存器、分步状态和ALERT中断。 */
void hsi_afe_init(hsi_afe_t *afe, const uint8_t *config);
/* 分步读取接口：每次调用执行一个AFE采集步骤，并与所有AFE寄存器写共用资源锁。 */
void hsi_afe_read(hsi_afe_t *afe);
/* 先处理挂起的ALERT，再执行一个分步采集步骤；整个服务过程持有AFE资源锁。 */
void hsi_afe_service(hsi_afe_t *afe);
/* AFE模块任务入口：首次调用完成初始化，后续调用执行一次分步采集。 */
void hsi_afe_task(void);
/* 请求单体均衡，并按配置串数裁剪均衡掩码。 */
bool hsi_afe_balance(hsi_afe_t *afe, uint32_t request);
/* 充电MOS直接控制封装。 */
void hsi_afe_setchgmos(hsi_afe_t *afe, MOS_State request);
/* 放电MOS直接控制封装。 */
void hsi_afe_setdsgmos(hsi_afe_t *afe, MOS_State request);
/* 充放电MOS组合控制封装，供高压管理模块使用。 */
void hsi_afe_setmos(hsi_afe_t *afe, MOS_State CMOS, MOS_State DMOS);
/* 清除指定AFE硬件故障标志。 */
bool hsi_afe_clear_error(hsi_afe_t *afe, uint8_t err_mask);
/* 将AFE切入低功耗模式；只有睡眠寄存器写入成功才返回true。 */
bool hsi_afe_lowpower(hsi_afe_t *afe);
/* 恢复AFE的IIC/ALERT接口并清除旧采样有效标志；返回接口恢复是否完成。 */
bool hsi_afe_normal(hsi_afe_t *afe);
/* 进入AFE关断/运输模式。 */
bool hsi_afe_shutdown(hsi_afe_t *afe);
/* 检查AFE复位状态。 */
bool hsi_afe_check(hsi_afe_t *afe);
/* 读取并解析AFE硬件状态寄存器；自动获取与清故障/控制共用的资源锁。 */
bool hsi_afe_status(m_soft_i2c_t *i2c, hsi_afe_status_t *afe_status);

/* 全局AFE对象，供任务、故障和MOS模块使用。 */
extern hsi_afe_t gt_hsi_afe;
#endif
