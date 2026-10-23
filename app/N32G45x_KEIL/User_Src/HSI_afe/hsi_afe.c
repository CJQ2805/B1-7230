/*
 * 文件作用：
 *   AFE硬件服务层，负责DVC1124初始化、分步采样、状态读取、MOS控制和低功耗控制。
 *
 * 主要用途：
 *   1. 周期性读取单体电压、电流、温度、VBAT和AFE状态。
 *   2. 处理AFE_ALERT事件并更新状态缓存。
 *   3. 封装DVC1124的MOS控制、均衡、清故障、休眠和唤醒。
 *
 * 适用范围：
 *   适用于当前DVC1124 AFE方案。后续如果更换AFE，应尽量保持对上层的AFE公共接口稳定。
 *
 * 调用关系：
 *   AFE_Task以10ms周期调用 hsi_afe_task()；底层通过软件IIC访问DVC1124；Board提供AFE相关IO。
 *
 * 注意事项：
 *   AFE读取采用分步方式，不能改成一次性长阻塞读取；公共头文件后续需要继续收敛，避免暴露DVC1124细节。
 */
#include "hsi_afe.h"
#include "d_exti.h"
#include "Driver/d_timer.h"
#include "App/app_bms_fault.h"
#include "Board/board_io.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "RTT/SEGGER_RTT.h"

/* 单体电压按物理通道分段采样；每次固定读取6路，AFE任务保持10ms调度。 */
#define HSI_AFE_CELL_READ_NUM_PER_STEP 6U
#define HSI_AFE_BALANCE_REFRESH_INTERVAL_MS 10000UL

/* DVC1124软件IIC实例，使用Board层提供的AFE引脚映射。 */
m_soft_i2c_t gt_hsi_afe_dvc1124_i2c =
{
	{
		{BOARD_IO_AFE_SCL_PORT, BOARD_IO_AFE_SCL_PIN},
		{BOARD_IO_AFE_SDA_PORT, BOARD_IO_AFE_SDA_PIN},
		{D_GPIO_PORT_INVALID, 0U, d_GPIO_MODE_INPUT, d_GPIO_NOPULL},
		{D_GPIO_PORT_INVALID, 0U, d_GPIO_MODE_OUTPUT_OD, d_GPIO_NOPULL}
	},
	false
};

/* 全局AFE实例；板级IO宏绑定原理图中的COFF、DOFF和ALERT引脚。 */
hsi_afe_t gt_hsi_afe =
{
	.ios = BOARD_IO_AFE_DVC1124_IOS_INIT,
	.i2c = &gt_hsi_afe_dvc1124_i2c,
	.cells = APP_BMS_CELL_COUNT,
	.cell_scan_count = APP_BMS_AFE_SCAN_CELL_COUNT
};

/* AFE任务初始化标志，避免应用层再包一层中间空壳。 */
static uint8_t s_hsi_afe_task_inited = 0U;
static SemaphoreHandle_t s_hsi_afe_mutex = NULL;
static uint32_t s_hsi_afe_occ2_diag_last_ms = 0UL;

bool hsi_afe_resource_init(void)
{
	if(s_hsi_afe_mutex == NULL)
	{
		s_hsi_afe_mutex = xSemaphoreCreateRecursiveMutex();
	}

	return (s_hsi_afe_mutex != NULL);
}

bool hsi_afe_acquire(uint32_t timeout_ms)
{
	TickType_t xTimeoutTicks;

	if(s_hsi_afe_mutex == NULL)
	{
		return false;
	}

	/* 创建任务前由main单线程完成初始化，不能在未启动调度器时阻塞。 */
	if(xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
	{
		return true;
	}

	xTimeoutTicks = (timeout_ms == 0UL) ? 0U : pdMS_TO_TICKS(timeout_ms);
	if((timeout_ms != 0UL) && (xTimeoutTicks == 0U))
	{
		xTimeoutTicks = 1U;
	}

	return (xSemaphoreTakeRecursive(s_hsi_afe_mutex, xTimeoutTicks) == pdTRUE);
}

void hsi_afe_release(void)
{
	if((s_hsi_afe_mutex != NULL) &&
	   (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
	{
		(void)xSemaphoreGiveRecursive(s_hsi_afe_mutex);
	}
}

/* AFE分步读取顺序：先扫描全部物理单体通道，再执行特殊采集步骤。 */
typedef enum
{
	HSI_AFE_TASK_CURRENT = APP_BMS_AFE_SCAN_CELL_COUNT,
	HSI_AFE_TASK_VBAT,
	HSI_AFE_TASK_TEMP,
	HSI_AFE_TASK_STATUS,
	HSI_AFE_TASK_END
} hsi_afe_task_t;

/* 将可选输出IO初始化为高电平；无效端口或零引脚直接忽略。 */
static void hsi_afe_io_output_high(d_io_t io)
{
	if((io.port == D_GPIO_PORT_INVALID) || (io.pin == 0U))
	{
		return;
	}

	d_gpio_init(io.port, io.pin, d_GPIO_MODE_OUTPUT_PP, d_GPIO_PULLUP);
	d_gpio_set_state(io.port, io.pin, 1);
}

/* ALERT外部中断回调：中断内只置位标志，IIC状态读取延后到hsi_afe_service()。 */
static void hsi_afe_alert_callback(void *object, uint8_t bit)
{
	hsi_afe_t *afe = (hsi_afe_t *)object;

	(void)bit;

	if(afe == 0)
	{
		return;
	}

	afe->alert_pending = 1U;
}

/* 原理图提供AFE_ALERT引脚时，将其配置为下降沿EXTI。 */
static void hsi_afe_alert_init(hsi_afe_t *afe)
{
	if((afe == 0) || (afe->ios.alert.port == D_GPIO_PORT_INVALID) || (afe->ios.alert.pin == 0U))
	{
		return;
	}

	d_exti_open(afe->ios.alert.port, afe->ios.alert.pin, d_GPIO_MODE_IT_FALLING,
	            d_GPIO_NOPULL, hsi_afe_alert_callback, afe);
}

/* 按配置的物理扫描路数限制读取范围，同时防止越过AFE缓冲区。 */
static uint8_t hsi_afe_cell_limit(hsi_afe_t *afe)
{
	if((afe->cell_scan_count == 0U) ||
	   (afe->cell_scan_count > APP_BMS_MAX_CELL_NUM))
	{
		return APP_BMS_AFE_SCAN_CELL_COUNT;
	}

	return afe->cell_scan_count;
}

/* 为本轮物理通道扫描生成完成校验掩码。 */
static uint32_t hsi_afe_cell_mask(uint8_t u8CellCount)
{
	if(u8CellCount >= 32U)
	{
		return 0xffffffffUL;
	}

	if(u8CellCount == 0U)
	{
		return 0U;
	}

	return (1UL << u8CellCount) - 1UL;
}

/* 生成本次分段读取覆盖的单体位图，用于判断一整轮电压是否已经读齐。 */
static uint32_t hsi_afe_cell_range_mask(uint8_t u8StartCellIndex, uint8_t u8CellCount)
{
	uint32_t u32CellMask = 0UL;
	uint8_t u8CellOffset;

	for(u8CellOffset = 0U; u8CellOffset < u8CellCount; u8CellOffset++)
	{
		if((u8StartCellIndex + u8CellOffset) >= APP_BMS_AFE_SCAN_CELL_COUNT)
		{
			break;
		}

		u32CellMask |= (1UL << (u8StartCellIndex + u8CellOffset));
	}

	return u32CellMask;
}

/* 推进非单体采集阶段的分步任务状态。 */
static void hsi_afe_next_task(hsi_afe_t *afe)
{
	afe->task++;
	if(afe->task >= HSI_AFE_TASK_END)
	{
		afe->task = 0;
	}
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : config - 保护参数
retval      : 无
function    : AFE服务初始化
**************************************************************************************/
void hsi_afe_init(hsi_afe_t *afe, const uint8_t *config)
{
	uint8_t u8InitResult;
	uint8_t u8InitAttempt;

	(void)config;

	if((afe == 0) || (afe->i2c == 0))
	{
		return;
	}

	for(u8InitAttempt = 0U; u8InitAttempt < 3U; u8InitAttempt++)//初始化最多三次
	{
		/* VDD5V引脚初始化，给芯片内部驱动供电 */
		hsi_afe_io_output_high(afe->ios.vdd5);
		/* 充电硬线关闭引脚初始化，默认上拉 */
		hsi_afe_io_output_high(afe->ios.chg_ctrl);
		/* 放电硬线关闭引脚初始化，默认上拉 */
		hsi_afe_io_output_high(afe->ios.dsg_ctl);

		/*芯片内部初始化*/
		u8InitResult = hsi_afe_dvc1124_Init(afe->i2c);
		if(u8InitResult == 1U)  break;//初始化成功就退出
	}
	afe->task = 0;
	afe->alert_pending = 0U;
	afe->cell_ready_mask = 0UL;
	afe->data_ready = 0U;
	afe->status_valid = 0U;
	/* AFE_NTC首次采样完成前按无效处理，不能把初始化零值当作正常温度。 */
	afe->temp[0] = INVALID_TEMPERATURE;
	/* 首次NTC寄存器读取完成前按IIC异常处理，避免误计入NTC电气异常。 */
	afe->ntc_status = HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT;
	/* ALERT独立于轮询处理，便于硬件故障快速刷新。 */
	hsi_afe_alert_init(afe);

}
/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : vol - 电压
num 串数
retval      : 无
function    : AFE读取电压，默认从首串开始读取，一般用于一次性全读
**************************************************************************************/
static void hsi_afe_vol(m_soft_i2c_t *ptI2c, uint16_t *pu16CellVoltageMv, uint8_t u8CellCount)
{
	uint16_t au16SampleVoltageMv[2][APP_BMS_MAX_CELL_NUM] = {0};
	uint16_t *pu16CompareVoltageMv;
	uint16_t u16VoltageDifferenceMv;
	uint8_t u8ValidationRoundCount;
	bool bSampleValid;

	u8CellCount = (u8CellCount > APP_BMS_MAX_CELL_NUM ? APP_BMS_MAX_CELL_NUM : u8CellCount);
	pu16CompareVoltageMv = pu16CellVoltageMv;//获取上一次数值
	u8ValidationRoundCount = sizeof(au16SampleVoltageMv) / APP_BMS_MAX_CELL_NUM / sizeof(uint16_t);

	for(uint8_t u8SampleIndex = 0U; u8SampleIndex < u8ValidationRoundCount; u8SampleIndex++)
	{
		//读IIC失败一次就退出
		if(0 != hsi_afe_dvc1124_CalcVolCell(ptI2c, au16SampleVoltageMv[u8SampleIndex], u8CellCount)) return;

		bSampleValid = true;
		for(uint8_t u8CellIndex = 0U; u8CellIndex < u8CellCount; u8CellIndex++)
		{
			if(pu16CompareVoltageMv[u8CellIndex] >= au16SampleVoltageMv[u8SampleIndex][u8CellIndex])
			{
				u16VoltageDifferenceMv = pu16CompareVoltageMv[u8CellIndex] - au16SampleVoltageMv[u8SampleIndex][u8CellIndex];
			}
			else
			{
				u16VoltageDifferenceMv = au16SampleVoltageMv[u8SampleIndex][u8CellIndex] - pu16CompareVoltageMv[u8CellIndex];
			}

			//差值大于1000mV，或者单体大于5500mV，表示无效值
			if((u16VoltageDifferenceMv >= 1000U) || (au16SampleVoltageMv[u8SampleIndex][u8CellIndex] >= 5500U))
			{
				/* 过滤明显跳变或不可能出现的单体电压数据。 */
				bSampleValid = false;
			}
			else
			{
				pu16CellVoltageMv[u8CellIndex] = au16SampleVoltageMv[u8SampleIndex][u8CellIndex];
			}
		}

		if(bSampleValid) return;

		pu16CompareVoltageMv = au16SampleVoltageMv[u8SampleIndex];
	}
}
/**************************************************************************************
date        : 2025.2.14
Author      : 
parameter   : seq 读取的电芯的序号  0~19
: num 往后连读的数量
retval      : 无
function    : AFE读电压服务，可以一次性全读，也可以读部分
**************************************************************************************/
static int8_t hsi_afe_vol_optional(m_soft_i2c_t *ptI2c, uint16_t *pu16CellVoltageMv, uint8_t u8StartCellIndex, uint8_t u8CellCount)
{
	uint16_t au16SampleVoltageMv[2][APP_BMS_MAX_CELL_NUM] = {0};
	uint16_t *pu16CompareVoltageMv;
	uint16_t u16VoltageDifferenceMv;
	uint8_t u8ValidationRoundCount;
	bool bSampleValid;

	if(u8StartCellIndex >= APP_BMS_MAX_CELL_NUM)
	{
		return -1;
	}

	/* 裁剪本次读取窗口，避免单次调用读出支持的单体数组范围。 */
	u8CellCount = (u8StartCellIndex + u8CellCount > APP_BMS_MAX_CELL_NUM ? APP_BMS_MAX_CELL_NUM - u8StartCellIndex : u8CellCount); //判断读取数量是否超过电芯总数
	pu16CompareVoltageMv = pu16CellVoltageMv;//获取上一次数值
	u8ValidationRoundCount = sizeof(au16SampleVoltageMv) / APP_BMS_MAX_CELL_NUM / sizeof(uint16_t);//有效性判断轮次

	for(uint8_t u8SampleIndex = 0U; u8SampleIndex < u8ValidationRoundCount; u8SampleIndex++)
	{
		//读IIC失败一次就退出
		if(0 != hsi_afe_dvc1124_CalcVolCell_Optional(ptI2c, au16SampleVoltageMv[u8SampleIndex], u8StartCellIndex, u8CellCount))
		{
			return -1;
		}

		bSampleValid = true;
		for(uint8_t u8CellIndex = u8StartCellIndex; u8CellIndex < (u8StartCellIndex + u8CellCount); u8CellIndex++)
		{
			if(pu16CompareVoltageMv[u8CellIndex] >= au16SampleVoltageMv[u8SampleIndex][u8CellIndex])
			{
				u16VoltageDifferenceMv = pu16CompareVoltageMv[u8CellIndex] - au16SampleVoltageMv[u8SampleIndex][u8CellIndex];
			}
			else
			{
				u16VoltageDifferenceMv = au16SampleVoltageMv[u8SampleIndex][u8CellIndex] - pu16CompareVoltageMv[u8CellIndex];
			}

			//差值大于1000mV，或者单体大于5500mV，表示无效值
			if((u16VoltageDifferenceMv >= 1000U) || (au16SampleVoltageMv[u8SampleIndex][u8CellIndex] >= 5500U))
			{
				/* 过滤明显跳变或不可能出现的单体电压数据。 */
				bSampleValid = false;
			}
			else
			{
				pu16CellVoltageMv[u8CellIndex] = au16SampleVoltageMv[u8SampleIndex][u8CellIndex];
			}
		}

		if(bSampleValid)
		{
			return 0;
		}

		pu16CompareVoltageMv = au16SampleVoltageMv[u8SampleIndex];
	}

	return -1;
}

/**************************************************************************************
date        : 2025.2.14
Author      : 
parameter   :
retval      : 无
function    : AFE读故障状态
**************************************************************************************/
static bool hsi_afe_status_unlocked(m_soft_i2c_t *i2c, hsi_afe_status_t *afe_status)
{
	bool bAfeStatusValid = true;

	if((i2c == 0) || (afe_status == 0))
	{
		return false;
	}

	if(hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0), 2))
	{
		/* R0/R1保存硬件保护标志和转换状态。 */
		afe_status->bits.chgov = gt_hsi_afe_dvc1124_regs.R0.bitmap.COV;
		afe_status->bits.dsguv = gt_hsi_afe_dvc1124_regs.R0.bitmap.CUV;
		afe_status->bits.dsgoc = ((gt_hsi_afe_dvc1124_regs.R0.bitmap.OCD1 != 0U) || (gt_hsi_afe_dvc1124_regs.R0.bitmap.OCD2 != 0U)) ? 1U : 0U;
		afe_status->bits.chgoc = ((gt_hsi_afe_dvc1124_regs.R0.bitmap.OCC1 != 0U) || (gt_hsi_afe_dvc1124_regs.R0.bitmap.OCC2 != 0U)) ? 1U : 0U;
		afe_status->bits.dsgoc2 = gt_hsi_afe_dvc1124_regs.R0.bitmap.OCD2;
		afe_status->bits.chgoc2 = gt_hsi_afe_dvc1124_regs.R0.bitmap.OCC2;
		afe_status->bits.sc = gt_hsi_afe_dvc1124_regs.R0.bitmap.SCD;
		afe_status->bits.iicwdgerr = gt_hsi_afe_dvc1124_regs.R0.bitmap.IWF;

		afe_status->bits.chipsta = gt_hsi_afe_dvc1124_regs.R1.CST;
		afe_status->bits.cc2finish = gt_hsi_afe_dvc1124_regs.R1.CC2F;
		afe_status->bits.cc1finish = gt_hsi_afe_dvc1124_regs.R1.CC1F;
		afe_status->bits.volfinish = gt_hsi_afe_dvc1124_regs.R1.VADF;
		afe_status->bits.chgdet = gt_hsi_afe_dvc1124_regs.R1.PD;

		/* 100A台架测试期间每秒输出一次关键量，避免RTT刷屏影响保护时序。 */
		{
			int32_t s32CurrentMa = gt_hsi_afe.curr_real;
			uint32_t u32NowMs;

			if ((s32CurrentMa >= 70000L) || (s32CurrentMa <= -70000L))
			{
				u32NowMs = d_timer_get_ms();
				if ((uint32_t)(u32NowMs - s_hsi_afe_occ2_diag_last_ms) >= 1000UL)
				{
					s_hsi_afe_occ2_diag_last_ms = u32NowMs;
					SEGGER_RTT_printf(0,
								  "AFE OCC2 I=%ldmA reg=%ld R0=%02X OCC2=%u R95=%02X R97=%02X\r\n",
								  (long)gt_hsi_afe.curr_real,
								  (long)gt_hsi_afe.curr_reg,
								  (unsigned int)AFE_REG_VALUE(R0),
								  (unsigned int)gt_hsi_afe_dvc1124_regs.R0.bitmap.OCC2,
								  (unsigned int)AFE_REG_VALUE(R95),
								  (unsigned int)AFE_REG_VALUE(R97));
				}
			}
		}
	}
	else
	{
		afe_status->bits.iicwdgerr = 1U;
		bAfeStatusValid = false;
	}

	if(hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(119), 2))
	{
		/* R119包含IIC看门狗超时状态。 */
		afe_status->bits.iicwdgerr = (afe_status->bits.iicwdgerr || gt_hsi_afe_dvc1124_regs.R119.IWTS) ? 1U : 0U;
	}
	else
	{
		afe_status->bits.iicwdgerr = 1U;
		bAfeStatusValid = false;
	}

	//充放电寄存器的信息在读电流CC2寄存器时顺便一起读取了
	afe_status->bits.chgmos = gt_hsi_afe_dvc1124_regs.R4_6.CHGF;
	afe_status->bits.dsgmos = gt_hsi_afe_dvc1124_regs.R4_6.DSGF;

	return bAfeStatusValid;
}

bool hsi_afe_status(m_soft_i2c_t *i2c, hsi_afe_status_t *afe_status)
{
	bool bAfeStatusValid;

	if((i2c == 0) || (afe_status == 0))
	{
		return false;
	}
	if(!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
	{
		return false;
	}

	bAfeStatusValid = hsi_afe_status_unlocked(i2c, afe_status);
	hsi_afe_release();
	return bAfeStatusValid;
}

/**************************************************************************************
date        : 2022-04-11
Author      : 
parameter   : vol - cell电压，通常传入一个数组，数组MinSize为16字节
: num - 电芯数量
: curr_real 电流真实值   curr_reg电流寄存值
: temp - 温度
: status - afe状态
retval      : 无
function    : AFE数据读取服务
**************************************************************************************/
static void hsi_afe_read_unlocked(hsi_afe_t *afe)
{
	uint8_t u8CellLimit;
	uint8_t u8ReadCellCount;
	uint32_t u32CellMask;

	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U))
	{
		return;
	}

	u8CellLimit = hsi_afe_cell_limit(afe);

	/* 分段读取单体电压，避免单次AFE 10ms任务阻塞太久。 */
	if((afe->task < u8CellLimit) && (afe->task < APP_BMS_AFE_SCAN_CELL_COUNT))
	{
		if(afe->task == 0U)
		{
			/* 新一轮单体分步采集开始，只清本轮位图，上一轮data_ready继续供应用层使用。 */
			afe->cell_ready_mask = 0UL;
		}

		u8ReadCellCount = u8CellLimit - afe->task;
		if(u8ReadCellCount > HSI_AFE_CELL_READ_NUM_PER_STEP)
		{
			u8ReadCellCount = HSI_AFE_CELL_READ_NUM_PER_STEP;
		}

		if(hsi_afe_vol_optional(afe->i2c, afe->vol, afe->task, u8ReadCellCount) == 0)
		{
			afe->cell_ready_mask |= hsi_afe_cell_range_mask(afe->task, u8ReadCellCount);
		}
		else
		{
			afe->data_ready = 0U;
		}
		afe->task += u8ReadCellCount;
		if(afe->task >= u8CellLimit)
		{
			u32CellMask = hsi_afe_cell_mask(u8CellLimit);
			afe->data_ready = ((afe->cell_ready_mask & u32CellMask) == u32CellMask) ? 1U : 0U;
			afe->task = HSI_AFE_TASK_CURRENT;
		}
		return;
	}

	if(afe->task < HSI_AFE_TASK_CURRENT)
	{
		afe->task = HSI_AFE_TASK_CURRENT;
	}

	switch(afe->task)
	{
		case HSI_AFE_TASK_CURRENT:
			/* 所有单体分段读取完成后，再读取电流。 */
			/* AFE只负责提供原始值，K/B校准和零死区由app_current_calib模块统一处理。 */
			if(hsi_afe_dvc1124_CalcCurrent(afe->i2c, &afe->curr_raw, &afe->curr_reg))
			{
				afe->curr_calibrated = afe->curr_raw;
				afe->curr_real = afe->curr_raw;
				/* 只有本次IIC读取成功，才通知校准模块有新电流样本。 */
				afe->current_update_seq++;
			}
			break;

		case HSI_AFE_TASK_VBAT:
			/* VBAT为AFE寄存器窗口中的电池端总压。 */
			hsi_afe_dvc1124_CalcVBAT(afe->i2c, &afe->vbat);
			break;

		case HSI_AFE_TASK_TEMP:
			/* 温度当前使用已配置的一个NTC通道。 */
			afe->ntc_status = hsi_afe_dvc1124_CalcTemp(afe->i2c, afe->temp);
			break;

		case HSI_AFE_TASK_STATUS:
			/* 即使ALERT未触发，也周期性轮询状态。 */
			afe->status_valid = hsi_afe_status_unlocked(afe->i2c, &afe->status) ? 1U : 0U;
			break;

		default:
			afe->task = 0;
			return;
	}

	hsi_afe_next_task(afe);

}

void hsi_afe_read(hsi_afe_t *afe)
{
	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U))
	{
		return;
	}
	if(!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
	{
		return;
	}
	if(afe->shutdown_requested == 0U)
	{
		hsi_afe_read_unlocked(afe);
	}
	hsi_afe_release();
}

void hsi_afe_service(hsi_afe_t *afe)
{
	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U))
	{
		return;
	}
	if(!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
	{
		return;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return;
	}

	/* ALERT状态读取和本轮采样共享同一锁，避免与清故障寄存器写入交错。 */
	if(afe->alert_pending != 0U)
	{
		/* 先处理ALERT，再做普通轮询，保证故障标志快速刷新。 */
		afe->alert_pending = 0U;
		afe->status_valid = hsi_afe_status_unlocked(afe->i2c, &afe->status) ? 1U : 0U;
	}

	/* 每次服务调用只执行一个分步读取步骤。 */
	hsi_afe_read_unlocked(afe);
	hsi_afe_release();
}

/* AFE模块统一任务入口：外部周期任务只需要调用这一处。 */
void hsi_afe_task(void)
{
	if(gt_hsi_afe.shutdown_requested != 0U)
	{
		return;
	}

	if(s_hsi_afe_task_inited == 0U)
	{
		hsi_afe_init(&gt_hsi_afe, 0);
		s_hsi_afe_task_inited = 1U;
	}

	hsi_afe_service(&gt_hsi_afe);
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : afe - 前端
: request - 均衡请求
retval      : 高8节和低8节都已设置
function    : dvc1124芯片固定机制决定在VADC的非工作期间自行轮换奇数串和偶数串的均衡。
**************************************************************************************/
static bool hsi_afe_balance_unlocked(hsi_afe_t *afe, uint32_t request)
{
	static uint32_t u32LastBalanceMask;
	static uint32_t u32LastBalanceWriteMs;
	bool bBalanceApplied = false;
	uint32_t u32NowMs;

	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U))
	{
		return false;
	}

	request &= hsi_afe_cell_mask(hsi_afe_cell_limit(afe));
	u32NowMs = d_timer_get_ms();

	if((request != u32LastBalanceMask) ||
	   ((request != 0UL) &&
	    ((uint32_t)(u32NowMs - u32LastBalanceWriteMs) >= HSI_AFE_BALANCE_REFRESH_INTERVAL_MS)))
	{
		if(1 == hsi_afe_dvc1124_Balance(afe->i2c, request))
		{
			u32LastBalanceMask = request;
			u32LastBalanceWriteMs = u32NowMs;
			afe->balstate = request;
			bBalanceApplied = true;
		}
	}
	else
	{
		/* 位图未变化且10秒刷新未到时跳过写入，保留当前均衡状态。 */
		afe->balstate = request;
		bBalanceApplied = true; //无需均衡&无需关闭
	}

	return bBalanceApplied;


}

bool hsi_afe_balance(hsi_afe_t *afe, uint32_t request)
{
	bool bBalanceResult;

	if(!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
	{
		return false;
	}

	bBalanceResult = hsi_afe_balance_unlocked(afe, request);
	hsi_afe_release();
	return bBalanceResult;
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : request - MOS状态
retval      : 无
function    : 设置充电MOS状态
**************************************************************************************/
void hsi_afe_setchgmos(hsi_afe_t *afe, MOS_State request)
{
	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return;
	}

	hsi_afe_dvc1124_ChMosCtl(afe->i2c, request);
	hsi_afe_release();
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : request - MOS状态
retval      : 无
function    : 设置放电MOS状态
**************************************************************************************/
void hsi_afe_setdsgmos(hsi_afe_t *afe, MOS_State request)
{
	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return;
	}

	hsi_afe_dvc1124_DchMosCtl(afe->i2c, request);
	hsi_afe_release();
}

/**************************************************************************************
date        : 2025-2-14
Author      : 
parameter   : request - MOS状态
retval      : 无
function    : 设置充放电MOS状态
**************************************************************************************/
void hsi_afe_setmos(hsi_afe_t *afe, MOS_State CMOS, MOS_State DMOS)
{
	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return;
	}

	/* 组合写入比单独设置两个MOS更省IIC访问次数。 */
	hsi_afe_dvc1124_MosCtl(afe->i2c, CMOS, DMOS);
	hsi_afe_release();
}

/**************************************************************************************
date        : 2024-11-29
Author      : 
parameter   : afe_err_mask - 需要清除的故障位
retval      : 无
function    : 清除AFE故障标志位
**************************************************************************************/
bool hsi_afe_clear_error(hsi_afe_t *afe, uint8_t err_mask)
{
	bool bClearResult;

	if((afe == 0) || (afe->i2c == 0) || (err_mask == AFE_MASK_NA))
	{
		return false;
	}
	if(afe->shutdown_requested != 0U)
	{
		return false;
	}
	if(!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS))
	{
		return false;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return false;
	}

	/* 移植驱动中DVC1124清故障命令使用取反后的掩码。 */
	err_mask = (uint8_t)(~err_mask);//应用层习惯置1为清理位，寄存器置0清理
	bClearResult = hsi_afe_dvc1124_CleanError(afe->i2c, err_mask);
	hsi_afe_release();
	return bClearResult;
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : 无
retval      : 无
function    : 设置AFE为低功耗模式（IDLE），在MCU休眠前调用，IIC通讯可唤醒AFE
**************************************************************************************/
bool hsi_afe_lowpower(hsi_afe_t *afe)
{
	bool bSleepCommandWritten;

	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return false;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return false;
	}

	bSleepCommandWritten = hsi_afe_dvc1124_Sleep(afe->i2c);
	hsi_afe_release();
	return bSleepCommandWritten;
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : 无
retval      : 无
function    : 设置AFE为工作状态
**************************************************************************************/
bool hsi_afe_normal(hsi_afe_t *afe)
{
	if((afe == 0) || (afe->i2c == 0))
	{
		return false;
	}

	/* STOP期间保留的缓存不能证明AFE已醒；先使旧采样全部失效。 */
	afe->task = 0U;
	afe->alert_pending = 0U;
	afe->cell_ready_mask = 0UL;
	afe->data_ready = 0U;
	afe->status_valid = 0U;
	afe->temp[0] = INVALID_TEMPERATURE;
	afe->ntc_status = HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT;

	if((afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return false;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return false;
	}

	m_soft_i2c_init(afe->i2c);
	/* 低功耗唤醒曾临时替换 ALERT 回调，这里恢复常规AFE故障回调。 */
	hsi_afe_alert_init(afe);
	hsi_afe_release();
	return true;
}

/**************************************************************************************
date        : 2024-11-11
Author      : 
parameter   : state - ship状态
retval      : 无
function    : 设置AFE SHIP状态
**************************************************************************************/
bool hsi_afe_shutdown(hsi_afe_t *afe)
{
	bool shutdown_written;

	if((afe == 0) || (afe->i2c == 0) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return false;
	}

	/* AFE进入SHIP前保存PF锁存来源，保证真正断电后上电仍能恢复PF状态。 */
	(void)app_bms_fault_save_shutdown_pf();
	/* 即使IIC写入失败也锁住后续软件访问；调用方可据返回值记录命令是否写成功。 */
	shutdown_written = hsi_afe_dvc1124_ShutDown(afe->i2c);
	afe->shutdown_requested = 1U;
	hsi_afe_release();
	return shutdown_written;
}

/**************************************************************************************
date        : 2024-11-11
Author      :
parameter   : state - ship状态
retval      : 无
function    : 设置AFE SHIP状态
**************************************************************************************/
bool hsi_afe_check(hsi_afe_t *afe)
{
	bool bAfeCheckPassed = false;

	if((afe == 0) || (afe->i2c == 0) || (afe->shutdown_requested != 0U) ||
	   (!hsi_afe_acquire(HSI_AFE_MUTEX_TIMEOUT_MS)))
	{
		return false;
	}
	if(afe->shutdown_requested != 0U)
	{
		hsi_afe_release();
		return false;
	}

	bAfeCheckPassed = hsi_afe_dvc1124_CheckRST(afe->i2c);

	hsi_afe_release();
	return bAfeCheckPassed;

}
