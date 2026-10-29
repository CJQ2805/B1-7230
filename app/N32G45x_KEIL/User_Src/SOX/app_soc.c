/**
 *  app_soc.c
 *  @Author: CJQ2805
 *  SOC handle packaging.
 *  For details please see app_soc_data_handle() Comments.
 *  Use the packaging invoking function app_soc_data_handle(),
 *  user need modification parameter in app_soc.h to
 *	fit their own projects.
 *	2022/11/16    10:31:11
 *  The file comments use UTF-8 encoding.
 */
/*
 * 本文件包含 SOC 的运行实现：
 *   SOC_Init                 首次默认初始化及 OCV 对齐；
 *   SOC_RealCurrentIntergrator 真实剩余容量的电流积分；
 *   SOC_DisplayCurrentIntergrator 显示容量积分及工况补偿；
 *   SOC_FCCHandle            满充确认、显示 SOC 平滑和欠压归零；
 *   SOC_FccUpdate/SOC_Stand  FCC 学习与长时间静置 OCV 校准；
 *   app_soc_data_handle      对外单周期调度入口。
 * 所有算法时间由 u32tim_250ms_cnt 驱动，普通 tick 为 250ms；电流方向为充电正、放电负。
 */
#include "app_soc.h"
#include "app_sop.h"
#include "stdlib.h"
#include "math.h"
#include "string.h"
struct _app_soc_t  gt_app_soc;
/* SOC运行状态的全局实例，由采样任务更新并由BMS快照/通信层读取。 */
/* 每次真实执行 OCV 校准时递增，供 BMS 任务识别查表校准和普通积分变化。 */
static uint32_t s_app_soc_ocv_update_count;

/*
 * 初始化 SOC 状态，必要时按 OCV 表校准容量。
 * 首次调用且没有有效 FCC 时建立设计容量对应的默认满电状态；后续只有强制校准，
 * 或 OCV 与当前积分容量偏差超过阈值时才重置真实/显示容量。display_soc 在这里
 * 不直接改写，由后续 FCC/SOC 处理阶段统一更新。
 */
static int SOC_Init(unsigned short u16arv_cellVolt, const unsigned short *pu16cell_volt_table, unsigned char* u8force_ocv_flag , long s32current)
{

	static unsigned char bSocInitialized = 0U; // 本次上电是否已完成至少一次有效初始化
	unsigned char u8CapacityBufferIndex;       // 初始化 FCC 五项平滑缓存时使用的数组下标
	int  s32SocDifferencePercent = 0;           // OCV 查表 SOC 与当前显示 SOC 的绝对差，单位百分点
	long s32RealCapacityDifference = 0L;        // OCV 容量与真实积分容量的绝对差，单位 mAh
	long s32DisplayCapacityDifference = 0L;     // OCV 容量与显示容量的绝对差，单位 mAh

	if(u16arv_cellVolt < 2000) // 最高单体低于有效电压下限，拒绝使用本次采样
	{
		/* 无效低压采样不能用来建立 SOC 状态。 */
		return -1;
	}

	/* 无有效Flash快照时，默认状态由SOC_Init自身建立；有效恢复值不被覆盖。 */
	if(gt_app_soc.s32battery_full_cap <= 0L) // FCC尚未从快照恢复或初始化，建立设计容量默认状态
	{
		gt_app_soc.s32battery_full_cap = SOC_DESIGN_CAP; // 尚无 FCC 时使用电池设计容量
		gt_app_soc.s32remain_cap = gt_app_soc.s32battery_full_cap; // 默认剩余容量为满容量
		gt_app_soc.tsoc_capacity_data.s32display_cap_val = gt_app_soc.s32battery_full_cap; // 显示容量同步为满
		gt_app_soc.tsoc_capacity_data.s32real_cap_val = gt_app_soc.s32battery_full_cap; // 真实积分容量同步为满
		gt_app_soc.u8display_soc = 100U; // 默认 SOC 为 100%
		gt_app_soc.u8display_soh = 100U; // 初始 SOH 按新电池处理
		for(u8CapacityBufferIndex = 0U; u8CapacityBufferIndex < 5U; u8CapacityBufferIndex++) // 初始化5项FCC学习滑动窗口
		{
			gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf[u8CapacityBufferIndex] =
				(unsigned short)gt_app_soc.s32battery_full_cap; // 用初始 FCC 填满历史缓存，避免初次平均偏低
		}
	}

	if((bSocInitialized != 0U) && (*u8force_ocv_flag == 0U)) // 已初始化且没有强制请求时跳过本次OCV校准
	{
		/* 正常运行中只做一次默认初始化；后续 OCV 校准必须由强制标志触发。 */
		return 0;
	}

	s32SocDifferencePercent = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table); // 当前最高单体的 OCV SOC
	s32SocDifferencePercent = (int)abs(s32SocDifferencePercent - gt_app_soc.u8display_soc); // 与显示 SOC 取绝对差

	s32RealCapacityDifference = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // OCV 对应容量
	s32RealCapacityDifference = (long)labs(s32RealCapacityDifference - gt_app_soc.tsoc_capacity_data.s32real_cap_val); // 与真实容量取差

	s32DisplayCapacityDifference = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // OCV 对应容量
	s32DisplayCapacityDifference = (long)labs(s32DisplayCapacityDifference - gt_app_soc.tsoc_capacity_data.s32display_cap_val); // 与显示容量取差

	//FCCUpdate
	gt_app_soc.tsoc_fcc_update.u8SocInit = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table); // 记录 FCC 学习窗口的 OCV 起点

	if((*u8force_ocv_flag != 0U) || // 收到明确的强制校准请求
	   (s32RealCapacityDifference > SOC_CAP_OFFSET_THRESHOLD) || // OCV容量与真实积分容量偏差超限
	   (s32SocDifferencePercent > SOC_OFFSET_THRESHOLD) || // OCV SOC与当前显示SOC偏差超限
	   (s32DisplayCapacityDifference > SOC_CAP_OFFSET_THRESHOLD)) // OCV容量与显示积分容量偏差超限
	{
//		gt_app_soc.u8display_soc  = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table);
		gt_app_soc.s32remain_cap = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // 对外剩余容量向 OCV 对齐
		gt_app_soc.tsoc_capacity_data.s32display_cap_val = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // 显示积分容量向 OCV 对齐
		gt_app_soc.tsoc_capacity_data.s32real_cap_val = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // 真实积分容量向 OCV 对齐

		*u8force_ocv_flag = 0; // 本次强制 OCV 请求已处理
		s_app_soc_ocv_update_count++; // 通知外部发生了一次 OCV 校准
	}

	bSocInitialized = 1U; // 记录已完成有效初始化，避免后续每周期重复校准

	return 0;
}



/*
 * 真实剩余容量的库仑计量积分器。输入电流单位 mA，按 250ms 节拍调用；累加
 * mA×250ms 刻度，达到 SOC_CAP_CNT_VAL（14400）换算为 1mAh，余数留待下周期。
 */
static void SOC_RealCurrentIntergrator(long s32current)
{
	long s32CapacityIncrement = 0L; // 本周期换算出的整数容量增量，单位 mAh，保留充放电符号
	int s32CurrentFilterLimit = SOC_MAX_CUR_FILTER_LIMIT_VUL; // 单周期容量增量滤波上限

	gt_app_soc.tsoc_capacity_data.s32cap_cnt += s32current; // 将本周期 mA 电流并入 250ms 积分余数
	s32CapacityIncrement = (long)(gt_app_soc.tsoc_capacity_data.s32cap_cnt / SOC_CAP_CNT_VAL); // 提取完整 mAh
	gt_app_soc.tsoc_capacity_data.s32cap_cnt = gt_app_soc.tsoc_capacity_data.s32cap_cnt - (long)s32CapacityIncrement * SOC_CAP_CNT_VAL; // 剩余小数刻度留到下周期

	/* 按电流方向分别记录充入/放出容量；总吞吐量始终累计绝对容量。 */
	if(s32CapacityIncrement >= 0) // 电流积分增量非负，记为充电吞吐量
	{
		gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap += labs(s32CapacityIncrement); // FCC 学习使用的总吞吐量
		gt_app_soc.tsoc_accumulate_cap_data.s32total_accumulate_charge_cap += labs(s32CapacityIncrement); // 生命周期累计充入量
		gt_app_soc.tsoc_accumulate_cap_data.s32single_accumulate_charge_cap += labs(s32CapacityIncrement); // 当前周期累计充入量
	}
	else
	{
		gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap += labs(s32CapacityIncrement); // 放电也按正吞吐量累计
		gt_app_soc.tsoc_accumulate_cap_data.s32total_accumulate_discharge_cap += s32CapacityIncrement; // 放电方向值保持负号
		gt_app_soc.tsoc_accumulate_cap_data.s32single_accumulate_discharge_cap += s32CapacityIncrement; // 当前周期放出量也保持负号
	}

	 gt_app_soc.u32cycle_index = (long)(gt_app_soc.tsoc_accumulate_cap_data.s32total_accumulate_charge_cap / gt_app_soc.s32battery_full_cap); // 累计充入 FCC 次数的整数部分

	if(s32CapacityIncrement > (-1)*s32CurrentFilterLimit && s32CapacityIncrement < s32CurrentFilterLimit) // 单周期容量增量处在尖峰过滤窗口内
	{
		gt_app_soc.tsoc_capacity_data.s32real_cap_val += s32CapacityIncrement; // 仅把通过尖峰限制的增量计入真实容量
		gt_app_soc.tsoc_fcc_update.dCycleCap += s32CapacityIncrement; // 同步累计 FCC 学习窗口内的净容量变化
	}

}

/*
 * 显示容量积分器：在真实电流积分基础上增加充电末端加速、放电带载压降补偿和
 * 低电压快速收敛。保留不足 1mAh 的积分余数，并将显示容量限制在 0~FCC。
 */
static void SOC_DisplayCurrentIntergrator(long s32current,unsigned short u16min_cellVolt,unsigned short u16arv_cellVolt, unsigned char u8ucv_flag ,unsigned char u8ocv_flag,const unsigned short *pu16cell_volt_table)
{
	long s32CapacityDifference = 0L, s32CapacityIncrement = 0L; // 真实/显示容量差与本周期显示容量增量
	static long s32DisplayCapacityRemainder = 0L; // 显示容量不足1mAh时保留的积分余数
	long  s32RealSocOffset = 0L /*, s32low_offset_soc = 0*/, s32JudgedSoc = 0L; // 带载偏移量及补偿后的判断SOC
//	unsigned char u8lookup_soc = 0;
	float fSocOffsetCoefficient = 0.0F; // 放电时的 SOC 压降补偿比例
	static unsigned long u32ExpediteTimerCount = 0UL; // 欠压附近加速收敛的计时起点
	static unsigned char bExpediteTimerState = 0U;	// 上述计时器是否已达到1秒
	int s32CurrentFilterLimit = SOC_MAX_CUR_FILTER_LIMIT_VUL; // 显示容量增量过滤上限

//	if(gt_app_soc.u8soc_fcc_flag)
//	{
//		gt_app_soc.tsoc_capacity_data.s32real_cap_val = gt_app_soc.s32battery_full_cap;
//	}

//	if(u8ucv_flag)
//	{
//		gt_app_soc.tsoc_capacity_data.s32real_cap_val = 0;
//	}


	if(1 == gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag) // 满充电压/电流窗口有效时加速真实容量积分
	{
		gt_app_soc.tsoc_capacity_data.s32real_cap_val += SOC_HUNDREDTH_CAP; // 满充窗口内快速把真实容量向上推近一个百分点
	}

	if(gt_app_soc.tsoc_capacity_data.s32real_cap_val > gt_app_soc.s32battery_full_cap) // 真实积分超过FCC时钳位
	{
		gt_app_soc.tsoc_capacity_data.s32real_cap_val = gt_app_soc.s32battery_full_cap; // 真实容量不能超过当前 FCC
	}

//	if(gt_app_soc.tsoc_capacity_data.s32real_cap_val < 0)
//	{
//		gt_app_soc.tsoc_capacity_data.s32real_cap_val = 0;
//	}

	if(s32current > SOC_CHAG_CURRENT_HANDLE) // 电流超过充电门限时按充电方向补偿显示容量
	{

		if(gt_app_soc.tsoc_capacity_data.s32real_cap_val > gt_app_soc.tsoc_capacity_data.s32display_cap_val + 3) // 真实容量领先显示容量超过3mAh
		{
			s32CapacityDifference = gt_app_soc.tsoc_capacity_data.s32real_cap_val - gt_app_soc.tsoc_capacity_data.s32display_cap_val; // 正值表示显示需要加快
		}
		else if(gt_app_soc.tsoc_capacity_data.s32real_cap_val + 3 < gt_app_soc.tsoc_capacity_data.s32display_cap_val) // 显示容量领先真实容量超过3mAh
		{
			s32CapacityDifference = gt_app_soc.tsoc_capacity_data.s32real_cap_val - gt_app_soc.tsoc_capacity_data.s32display_cap_val; // 负值表示显示需要减慢
		}
		else
		{
			s32CapacityDifference = 0L; // 容量差在±3mAh死区内，不额外追赶
		}


		if(s32CapacityDifference > 0) // 真实容量高于显示容量时，提高显示积分速度
		{

			if((u16arv_cellVolt > pu16cell_volt_table[SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC])&&(gt_app_soc.u8display_soc < SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC+5)) // 高于高SOC OCV电压且显示SOC仍在末端窗口
			{
				s32current = s32current + s32current; // 充电末端大幅加快显示容量增长
			}
			else
			{
				s32current = s32current + s32current/5; // 真实容量领先时通常增加20%的显示积分速度
			}
		}
		else
		{
			s32current = s32current - s32current /5; // 显示容量领先时降低20%的积分速度
		}


		if((u16arv_cellVolt < (pu16cell_volt_table[SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC] /*+ (s32current * )*/))&&(gt_app_soc.u8display_soc >= SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC)) // SOC已到高SOC区但电压未达对应表项，减缓充电显示速度
		{
			s32current = s32current - s32current / 3; // OCV电压未达到高SOC阈值时抑制显示SOC继续快速上涨
		}
		else if((u16arv_cellVolt > pu16cell_volt_table[SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC])&&(gt_app_soc.u8display_soc < SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC + 5)) // 电压越过高SOC表项且SOC尚在末端区，额外加速
		{
			s32current = s32current + s32current / 2; // 高电压且SOC仍处末端窗口时额外加速
		}

		if((1 == gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag)) // 满充窗口成立时再叠加固定末端补偿
		{
			s32current = s32current + (SOC_HUNDREDTH_CAP/SOC_CHAG_HIGH_SPEED_PERCENT_SECOND*2); // 满充条件成立时额外注入末端加速量
		}

	}
	else if(s32current < SOC_DISCHRG_CHRRCENT_HANDLE) // 电流低于放电门限时按放电方向补偿
	{

		  s32current = -s32current; // 临时转成正放电量进行压降补偿计算

		//stock soc handle mode   discharge
		s32RealSocOffset = (gt_app_soc.tsoc_compensate_para.s8real_soc - gt_app_soc.tsoc_compensate_para.s8stock_soc ); // 真实SOC与带载估算SOC的偏差

		s32JudgedSoc = (long)gt_app_soc.u8display_soc - s32RealSocOffset; // 扣除带载偏移，得到当前应追踪的显示SOC

		if((gt_app_soc.tsoc_compensate_para.s8real_soc - gt_app_soc.u8display_soc) >= gt_app_soc.tsoc_compensate_para.s8stock_soc) // 压降偏移大于显示SOC可覆盖范围时将目标SOC设为0
		{
			s32JudgedSoc = 0L; // 压降偏移已覆盖剩余显示SOC时，不允许目标SOC为负
		}

		if((int)gt_app_soc.u8display_soc - gt_app_soc.tsoc_compensate_para.s8stock_soc > 0) // 显示SOC减去静态压降偏移后仍大于0时计算比例
		{
			fSocOffsetCoefficient = (float)s32JudgedSoc /(float)(gt_app_soc.u8display_soc - gt_app_soc.tsoc_compensate_para.s8stock_soc); // 按剩余SOC比例修正放电积分
		}
		else
		{
			fSocOffsetCoefficient = (s32JudgedSoc + gt_app_soc.u8display_soc) / gt_app_soc.u8display_soc; // 分母不为正时使用备用比例表达式
		}

		if(s32RealSocOffset < gt_app_soc.u8display_soc ) // 偏移未完全覆盖显示SOC时应用放电补偿系数
		{
			s32current = s32current + s32current * fSocOffsetCoefficient; // 增大放电积分，使显示SOC按负载压降更快下降
		}




//////////////////////////////////////////////////////////
		//大电池屏蔽，小电池启用
//		 s32low_offset_soc = (char)(SOC_DISCHG_HIGH_SPEED_VOLTAGE_SOC);
//
//		 if((u16min_cellVolt < (pu16cell_volt_table[SOC_DISCHG_HIGH_SPEED_VOLTAGE_SOC])) && \
//			(gt_app_soc.u8display_soc > (s32low_offset_soc + gt_app_soc.tsoc_compensate_para.s8stock_soc)))
//		 {
//			s32current = s32current + s32current ;
//		 }



		bExpediteTimerState = app_soc_delay(1, // judge恒为真，按250ms节拍等待1秒
							  &u32ExpediteTimerCount, // 欠压附近快速收敛计时起点
							  1 * 4, // 1秒换算成4个250ms tick
							  bExpediteTimerState, // 上一次计时状态
							  1); // 满1秒时置位事件


		 if(1 == bExpediteTimerState) // 1秒事件到期后执行一次低压加速检查
		 {
			  bExpediteTimerState = 0; // 消费本次到期事件，开始下一次1秒计时

//		 	if((u16min_cellVolt < SOC_DISCHG_CUV_VOLTAGE + 250)  && gt_app_soc.u8display_soc > 0)
//			{
//				s32current = s32current + s32current*2;
//			}

			if((u16min_cellVolt < SOC_DSG_CUV_VOLTAGE + 100) && gt_app_soc.u8display_soc > 0) // 最低单体进入欠压阈值上方100mV以内且SOC尚未归零
			{
				s32current = s32current + SOC_HUNDREDTH_CAP; // 最低单体接近欠压时额外增加1%容量的放电补偿
			}
		 }

////////////////////////////////////////////////////////
		//lookup table compensation mode
//		 u8lookup_soc = app_soc_voltage_to_soc(u16min_cellVolt, pu16cell_volt_table);
//
//		if(gt_app_soc.u8display_soc > u8lookup_soc)
//		{
//
//		}

		s32current = -s32current; // 恢复负号，让后续积分按放电方向减少显示容量
	}

	s32DisplayCapacityRemainder += s32current; // 累加补偿后的带符号电流
	s32CapacityIncrement = (long)(s32DisplayCapacityRemainder / SOC_CAP_CNT_VAL); // 换算本周期整数mAh
	s32DisplayCapacityRemainder = s32DisplayCapacityRemainder - (long)(s32CapacityIncrement)*SOC_CAP_CNT_VAL; // 保存未满1mAh的余量

	if(s32CapacityIncrement > (-1)*s32CurrentFilterLimit && s32CapacityIncrement < s32CurrentFilterLimit) // 本周期显示容量增量通过尖峰限制
	{
		gt_app_soc.tsoc_capacity_data.s32display_cap_val += s32CapacityIncrement; // 过滤通过后更新显示容量
	}

	if(gt_app_soc.tsoc_capacity_data.s32display_cap_val < 0) // 显示容量不能低于0
	{
		gt_app_soc.tsoc_capacity_data.s32display_cap_val = 0; // 显示容量下限为0
	}

	if(gt_app_soc.tsoc_capacity_data.s32display_cap_val > gt_app_soc.s32battery_full_cap) // 显示容量不能高于FCC
	{
		gt_app_soc.tsoc_capacity_data.s32display_cap_val	= gt_app_soc.s32battery_full_cap; // 显示容量上限为当前FCC
	}

}


/*
 * SOC 电流积分调度入口。正常运行仅在 250ms 节拍到达时积分并清除输入标志；
 * 休眠补算时按 STAND_SOC_SLEEP_DELAY_TIME 推进内部时基。积分前扣除板级静态电流，
 * 再根据温度、电流和内阻估算带载压降 SOC，分别更新真实容量与显示容量。
 */
static void SOC_CurrentIntegrator(long s32current, unsigned char* au8tim_250ms_flag,unsigned short u16min_cellVolt , unsigned short u16arv_cellVolt, unsigned char u8ucv_flag,unsigned char u8ocv_flag, const unsigned short *pu16cell_volt_table, int s16cell_min_temp,unsigned char u8sleep_flag)
{
	double fVoltageOffsetMv = 0.0; // 用于 OCV 表查询的带载等效电压，单位 mV
	double fLowTemperatureResistance = 0.0; // 按温度/电流得到的等效内阻，单位 mΩ
	long  s32AbsoluteCurrentMa = 0L; // 扣除静态耗电后的电流幅值，单位 mA
	if((!*au8tim_250ms_flag) && (!u8sleep_flag)) // 普通模式无新tick时直接跳过，睡眠补算模式除外
	return; // 普通运行且本周期没有时间标志时跳过，避免重复积分

	if(!u8sleep_flag) // 普通运行按一次250ms周期推进时基
	{
		gt_app_soc.u32tim_250ms_cnt++; // 正常采样周期推进一个250ms tick
		*au8tim_250ms_flag = 0; // 本次时间标志已消费
	}
	else
	{
		gt_app_soc.u32tim_250ms_cnt+= STAND_SOC_SLEEP_DELAY_TIME; // 休眠补算按宏定义步长推进节拍
	}


	s32current -= SOC_CURRENT_SELF_CONSUME; // 去掉 MCU/BMS 板级静态电流
	s32AbsoluteCurrentMa = labs(s32current); // 内阻压降只使用电流幅值

	if(s16cell_min_temp <= 10) // 低温区使用随电流变化的经验内阻模型
	{
		fLowTemperatureResistance = -0.01 * s32AbsoluteCurrentMa + 151; // 10℃及以下使用随电流变化的经验内阻
	}
	else
	{
		fLowTemperatureResistance = SOC_COMPENSATE_RESISTANCE; // 高于10℃时用固定平均内阻
	}

	if(fLowTemperatureResistance < SOC_COMPENSATE_RESISTANCE) // 经验结果低于基础内阻时提升到基础值
	{
		fLowTemperatureResistance = SOC_COMPENSATE_RESISTANCE; // 经验公式结果不得低于配置的基础内阻
	}


	fVoltageOffsetMv = (s32AbsoluteCurrentMa*fLowTemperatureResistance/1000) + pu16cell_volt_table[0]; // 用 I×R 压降构造等效 OCV 电压
	gt_app_soc.tsoc_compensate_para.s8stock_soc =  app_soc_voltage_to_soc(fVoltageOffsetMv, pu16cell_volt_table); // 等效电压查表得到带载 SOC 偏移
	if(gt_app_soc.tsoc_compensate_para.s8stock_soc > 15) // 限制压降估算产生的SOC偏移上限
	{
		gt_app_soc.tsoc_compensate_para.s8stock_soc = 15; // 限制补偿不超过15个百分点
	}
	gt_app_soc.tsoc_compensate_para.s8real_soc	 =  (unsigned char)((gt_app_soc.tsoc_capacity_data.s32real_cap_val*100/ gt_app_soc.s32battery_full_cap)); // 真实容量转百分比供放电补偿使用

	SOC_RealCurrentIntergrator(s32current); // 更新未做显示补偿的真实容量
	SOC_DisplayCurrentIntergrator(s32current,u16min_cellVolt,u16arv_cellVolt, u8ucv_flag, u8ocv_flag,pu16cell_volt_table); // 更新平滑/补偿后的显示容量

}


/*
 * 满充确认、SOC 显示平滑和欠压收敛处理。
 * FCC_SET_MODE 选择 CC/CV 满充判据；满充确认后容量/SOC 同步到 100%，解除判据成立
 * 后清除满充标志。正常充放电按定时步长追赶显示容量，欠压标志最终将显示值归零。
 */
void SOC_FCCHandle(long s32current, unsigned int u32cell_arv_volt	,unsigned char u8ucv_flag, unsigned char u8ocv_flag,unsigned int u32cell_min_volt)
{
	static unsigned long u32fcc_set_cnt = 0, u32fcc_reset_cnt = 0; // 满充置位/解除判据的250ms计时起点
	static unsigned long u32soc_tim_cnt = 0, u32soc_10min_tim_cnt = 0; // SOC快速/慢速追赶的计时起点
	static unsigned char bsoc_change_tim = 0, bsoc_change_10min_tim = 0; // 1秒/10分钟追赶到期状态


//	if(gt_app_soc.tsoc_capacity_data.s32display_cap_val <= 0)
//	{
//		u32remain_cap_temp = 0;
//	}
//	else if(gt_app_soc.tsoc_capacity_data.s32display_cap_val >= gt_app_soc.s32battery_full_cap)
//	{
//		u32remain_cap_temp = gt_app_soc.s32battery_full_cap;
//	}
//	else
//	{
//		u32remain_cap_temp = gt_app_soc.tsoc_capacity_data.s32display_cap_val;
//	}

//	s32tmp = (long)gt_app_soc.u32remain_cap * 1000;
//	s32tmp = s32tmp / gt_app_soc.s32battery_full_cap;

//	if(s32tmp <= 0)
//		gt_app_soc.tsoc_fcc_data.u8handle_soc = 0;
//	else
//		gt_app_soc.tsoc_fcc_data.u8handle_soc = s32tmp /10 +1;


	#if FCC_SET_MODE // 编译期选择：1为CV电流+电压窗口模式，0为CC电压模式
	//CV mode
	if(SOC_FCC_ACCELERATE_CONDITION) // CV满充窗口成立，允许积分器加速逼近满容量
	{
		gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag = 1; // 满充加速条件有效
	}
	else
	{
		gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag = 0; // 离开满充窗口后停止加速
	}

	 gt_app_soc.u8soc_fcc_flag = app_soc_delay((((s32current > S0C_FCC_SET_STOP_CURRENT_MIN) && (s32current < SOC_FCC_SET_STOP_CURRENT_MAX) && (u32cell_arv_volt > SOC_FCC_SET_MIN_CELL_VOLTAGE))), // CV窗口持续到配置时间后置满充标志
																	&u32fcc_set_cnt, // 保存满充置位计时起点
																	(SOC_FCC_SET_DELAY_TIME * 4), // 满充延时秒数换算为250ms tick
																	gt_app_soc.u8soc_fcc_flag, // 当前满充锁存状态
																	1); // 条件持续到期后置位满充状态


#else

	//CC mode
	if(u32cell_max_volt > (SOC_FCC_SET_VOLTAGE - 20)) // CC模式下，最高单体进入设定电压附近时启用满充加速
	{
		gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag = 1; // 满充电压窗口有效
		if((gt_app_soc.tsoc_fcc_data.u8handle_soc >= 99) && (gt_app_soc.u8soc_fcc_flag == 0)) // 满充未确认且内部SOC已到99%时暂留在99%
		{
				gt_app_soc.tsoc_fcc_data.u8handle_soc = 99; // 等待满充延时确认，不提前显示100%
		}
	}
	else
	{
		gt_app_soc.tsoc_fcc_data.u8fcc_condition_flag = 0; // 离开满充电压窗口后停止满充加速
		if((gt_app_soc.tsoc_fcc_data.u8handle_soc >= 99) && (s32current > S0C_FCC_SET_STOP_CURRENT_MIN)) // 仍有充电电流时不让内部SOC越过99%
		{
			gt_app_soc.tsoc_fcc_data.u8handle_soc = 99; // 维持待确认状态
		}
	}

	gt_app_soc.u8soc_fcc_flag = app_soc_delay((u32cell_max_volt > SOC_FCC_SET_VOLTAGE), // 最高单体超过CC满充阈值时开始计时
																	&u32fcc_set_cnt, // 保存本轮满充计时起点
																	SOC_FCC_SET_DELAY_TIME * 4, // 秒换算为250ms tick
			gt_app_soc.u8soc_fcc_flag, // 当前满充锁存状态
																	1); // 到期后置位满充状态

#endif


	gt_app_soc.u8soc_fcc_flag = app_soc_delay((u32cell_arv_volt < SOC_FCC_RESET_MIN_CELL_VOLTAGE) || (s32current < SOC_FCC_RESET_CURRENT), // 最高单体低于复位阈值或放电电流越过阈值时开始解除计时
			&u32fcc_reset_cnt, // 保存解除条件的计时起点
			SOC_FCC_RESET_DELAY_TIME * 4, // 配置秒数换算为250ms tick
			gt_app_soc.u8soc_fcc_flag, // 当前满充状态
			0); // 条件持续到期后清除满充状态

	if(gt_app_soc.u8soc_fcc_flag || u8ocv_flag) // 满充标志已置位或输入过压标志有效时，按满充状态同步各容量值
	{
		gt_app_soc.u8soc_fcc_flag = 1; // 锁存满充状态
		gt_app_soc.tsoc_fcc_data.u8handle_soc = 100; // CC模式的内部满充SOC记录
		gt_app_soc.u8display_soc = 100; // 对外SOC立即置满
		gt_app_soc.tsoc_capacity_data.s32display_cap_val	= gt_app_soc.s32battery_full_cap; // 显示容量置为FCC
		gt_app_soc.tsoc_capacity_data.s32real_cap_val = gt_app_soc.s32battery_full_cap; // 真实容量同步到FCC
		gt_app_soc.s32remain_cap = gt_app_soc.s32battery_full_cap; // 剩余容量同步到FCC
	}

	if ((gt_app_soc.s32remain_cap <= (gt_app_soc.s32battery_full_cap*3/200)) && (u32cell_min_volt > SOC_DISCHG_KEEP_ONE_VOLTAGE)) // 剩余容量低于1.5%且最低单体仍高于保留电压
	{
		if (s32current < SOC_DISCHRG_CHRRCENT_HANDLE) // 仅在有效放电时执行容量底部处理
		{
			if(gt_app_soc.u8display_soc == 0) // 显示SOC已归零，允许容量同时归零
			{
				gt_app_soc.s32remain_cap = 0; // 显示SOC已为0时允许剩余容量归零
				gt_app_soc.tsoc_capacity_data.s32display_cap_val = 0; // 显示容量同步归零
			}
			else // 尚未显示0%，保留底部容量，避免SOC提前归零
			{
			gt_app_soc.s32remain_cap = (gt_app_soc.s32battery_full_cap *3/200); // 尚未显示0%时保留1.5%容量
			gt_app_soc.tsoc_capacity_data.s32display_cap_val = (gt_app_soc.s32battery_full_cap *3/200); // 显示容量同步保留值
			}
		}
	}

	bsoc_change_tim = app_soc_delay(1, // judge恒为真，按配置间隔产生快速追赶机会
							  &u32soc_tim_cnt, // 快速追赶计时起点
							  SOC_HOW_LONG_SECOND_ALLOW_ADD_ONE * 4, // 秒换算为250ms tick
							  bsoc_change_tim, // 上次追赶到期状态
							  1); // 到期后置位追赶事件

	bsoc_change_10min_tim = app_soc_delay(1, // 10分钟低频追赶定时器，用于校正小偏差
							  &u32soc_10min_tim_cnt, // 慢速追赶计时起点
							  2400, // 10分钟对应2400个250ms tick
							  bsoc_change_10min_tim, // 上次慢速追赶到期状态
							  1); // 到期后置位慢速追赶事件

//	bremain_cap_change_tim = app_soc_delay(1,
//							  &u32remain_tim_cnt,
//							  SOC_HOW_LONG_SECOND_ALLOW_ADD_ONE * 4,
//							  bremain_cap_change_tim,
//							  1);

//	bremain_cap_change_1min_tim = app_soc_delay(1,
//							  &u32remain_1min_tim_cnt,
//							  240,
//							  bremain_cap_change_1min_tim,
//							  1);


//	/*追剩余容量*/
//	if((s32current >= SOC_CHAG_CURRENT_HANDLE) && (gt_app_soc.u8display_soc >= ((gt_app_soc.s32remain_cap*100)/gt_app_soc.s32battery_full_cap)) && (bremain_cap_change_tim == 1))
//	{
//		bremain_cap_change_tim = 0;
//		bremain_cap_change_1min_tim = 0;
//		u32remain_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		u32remain_1min_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		if(gt_app_soc.s32remain_cap < gt_app_soc.tsoc_capacity_data.s32display_cap_val)
//		{
//			gt_app_soc.s32remain_cap +=10;
//		}
//		if( gt_app_soc.s32remain_cap > gt_app_soc.s32battery_full_cap)
//		{
//			gt_app_soc.s32remain_cap = gt_app_soc.s32battery_full_cap;
//		}
//
//	}
//
//	if((s32current <= SOC_DISCHRG_CHRRCENT_HANDLE) && (gt_app_soc.u8display_soc <= ((gt_app_soc.s32remain_cap*100)/gt_app_soc.s32battery_full_cap) ) && (bremain_cap_change_tim == 1))
//	{
//		bremain_cap_change_tim = 0;
//		bremain_cap_change_1min_tim = 0;
//		u32remain_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		u32remain_1min_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		if((gt_app_soc.s32remain_cap >= 10 ) && (gt_app_soc.s32remain_cap > gt_app_soc.tsoc_capacity_data.s32display_cap_val))
//		{
//			gt_app_soc.s32remain_cap -=10;
//		}
//	}
//
//		/*1min去追剩余容量的值*/
//	if( ((s32current > SOC_DISCHRG_CHRRCENT_HANDLE) && (s32current < 5) )
//		&& (gt_app_soc.u8display_soc < ((gt_app_soc.s32remain_cap*100)/gt_app_soc.s32battery_full_cap)) && (1 == bremain_cap_change_1min_tim))
//	{
//		bremain_cap_change_tim = 0;
//		bremain_cap_change_1min_tim = 0;
//		u32remain_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		u32remain_1min_tim_cnt = gt_app_soc.u32tim_250ms_cnt;
//		if(gt_app_soc.s32remain_cap >= 10 )
//		{
//			gt_app_soc.s32remain_cap -=10;
//		}
//	}

	/*追SOC*/
	if((s32current >= SOC_CHAG_CURRENT_HANDLE) && (gt_app_soc.s32remain_cap < gt_app_soc.tsoc_capacity_data.s32display_cap_val) && (1 == bsoc_change_tim)) // 充电且真实剩余容量低于显示积分容量时向上追赶
	{
		bsoc_change_tim = 0; // 消费本次快速追赶事件
		bsoc_change_10min_tim = 0; // 重置慢速追赶状态，避免同一周期重复修正
		u32soc_10min_tim_cnt = gt_app_soc.u32tim_250ms_cnt; // 两个计时器均从当前时刻重新计时
		u32soc_tim_cnt = gt_app_soc.u32tim_250ms_cnt;

		gt_app_soc.s32remain_cap += 10; // 先增加10mAh的小步长

		if((gt_app_soc.tsoc_capacity_data.s32display_cap_val - gt_app_soc.s32remain_cap) > (gt_app_soc.s32battery_full_cap/100/5)) // 偏差大于0.2% FCC时再做较大步进
		{
			gt_app_soc.s32remain_cap += gt_app_soc.s32battery_full_cap/100/5; // 偏差仍超过0.2% FCC时再增加0.2% FCC
		}

		if(gt_app_soc.s32remain_cap >= gt_app_soc.s32battery_full_cap) // 追赶后检查满容量边界
		{
			gt_app_soc.s32remain_cap = gt_app_soc.s32battery_full_cap; // 快速追赶不能超过FCC
		}
	}

	if((s32current <= SOC_DISCHRG_CHRRCENT_HANDLE) && (gt_app_soc.s32remain_cap > gt_app_soc.tsoc_capacity_data.s32display_cap_val)&& (1 == bsoc_change_tim)) // 放电且真实剩余容量高于显示积分容量时向下追赶
	{
		bsoc_change_tim = 0; // 消费本次快速追赶事件
		bsoc_change_10min_tim = 0; // 同步重置慢速追赶计时
		u32soc_10min_tim_cnt = gt_app_soc.u32tim_250ms_cnt; // 从本周期重新计算两个延时
		u32soc_tim_cnt = gt_app_soc.u32tim_250ms_cnt;

		gt_app_soc.s32remain_cap -= 10; // 先减少10mAh小步长

		if((gt_app_soc.s32remain_cap - gt_app_soc.tsoc_capacity_data.s32display_cap_val) > (gt_app_soc.s32battery_full_cap/100/5)) // 偏差大于0.2% FCC时再做较大步进
		{
			gt_app_soc.s32remain_cap -= gt_app_soc.s32battery_full_cap/100/5; // 偏差较大时额外减少0.2% FCC
		}

		if(gt_app_soc.s32remain_cap <= 0) // 追赶后检查容量下限
		{
			gt_app_soc.s32remain_cap = 0; // 防止减量后出现负容量
		}

	}

	/* 轻载/静置且偏差超过1% FCC时，每10分钟额外向显示积分容量追赶一次。 */
	/*10min去追SOC的值*/
	if( ((s32current > SOC_DISCHRG_CHRRCENT_HANDLE) && (s32current < 50) ) // 电流位于轻载/静置窗口
		&&((gt_app_soc.s32remain_cap  - gt_app_soc.tsoc_capacity_data.s32display_cap_val) > (gt_app_soc.s32battery_full_cap/100))&&(1 == bsoc_change_10min_tim)) // 剩余容量比显示容量高出1% FCC且10分钟计时到期
	{
		bsoc_change_tim = 0; // 本次慢速校准结束后重置短周期定时状态
		bsoc_change_10min_tim = 0; // 重新开始10分钟计时
		u32soc_10min_tim_cnt = gt_app_soc.u32tim_250ms_cnt; // 两个延时都从当前时刻开始
		u32soc_tim_cnt = gt_app_soc.u32tim_250ms_cnt;

		if(( gt_app_soc.s32remain_cap - gt_app_soc.tsoc_capacity_data.s32display_cap_val) > (gt_app_soc.s32battery_full_cap/100)) // 再确认偏差仍大于1% FCC
		{
			gt_app_soc.s32remain_cap -= gt_app_soc.s32battery_full_cap/100; // 慢速校正一次减少1% FCC
		}

		if(gt_app_soc.s32remain_cap <= 0) // 慢速校正后检查容量下限
		{
			gt_app_soc.s32remain_cap = 0; // 容量下限钳位
		}
	}

	/* 所有容量追赶结束后再限幅，防止未满充时每周期越限后又被压回。 */
	if ((!gt_app_soc.u8soc_fcc_flag) && (!u8ocv_flag) &&
	    (gt_app_soc.s32remain_cap > gt_app_soc.s32battery_full_cap * 199 / 200))
	{
		gt_app_soc.s32remain_cap = gt_app_soc.s32battery_full_cap * 199 / 200;
	}

	gt_app_soc.u8display_soc = gt_app_soc.s32remain_cap*100/gt_app_soc.s32battery_full_cap; // 由显示剩余容量换算整数百分比 SOC

	if(u8ucv_flag) // 本周期欠压标志有效时，最终对外SOC和显示容量强制清零
	{
		gt_app_soc.u8display_soc = 0; // 欠压状态将显示 SOC 强制归零
		gt_app_soc.tsoc_capacity_data.s32display_cap_val = 0; // 显示积分容量同步归零
		gt_app_soc.s32remain_cap = 0; // 对外剩余容量同步归零
	}

}



/*
 * FCC 学习与 SOH 估算：轻载持续 20 分钟后采样一次 OCV SOC；SOC 窗口变化超过
 * 40 个百分点时，用窗口净吞吐量/SOC变化比例估算 FCC。有效结果进入 5 项缓存平均，
 * 并限制在当前 FCC 的 90%~105% 和设计容量范围内。
 */
static void SOC_FccUpdate(unsigned short u16arv_cellVolt, const unsigned short *pu16cell_volt_table, long s32current)
{
	unsigned char u8SocDifference = 0U; // FCC 学习窗口首尾 OCV SOC 的差值，单位百分点
	signed char s8SocTemporary = 0; // 当前 OCV 查表 SOC
	unsigned int u32CalculatedCapacity = 0U; // 由电量吞吐量反推的 FCC，单位 mAh
	int s32CapacitySum = 0; // 5 个 FCC 缓存值的求和中间量
	static unsigned long u32UpdateCount = 0UL; // 20分钟轻载计时起点
	static unsigned char bTwentyMinuteTimer = 0U; // 20分钟学习周期到期标志
	static char bSocInitializationUpdated = 0; // 当前 FCC 学习周期是否已记录起点 SOC
	uint8_t u8CurrentSoc = 0U; // 用于识别电池是否回到0%端点

	bTwentyMinuteTimer = app_soc_delay(s32current < 200 && // 充放电电流小于+200mA
	                        s32current > -200, // 且大于-200mA，作为轻载静置窗口
						&u32UpdateCount, // 保存FCC学习计时起点
	                        4800, // 20分钟换算为4800个250ms tick
						bTwentyMinuteTimer, // 上次计时到期状态
	                        1); // 连续轻载到期后置位学习标志

	if(bTwentyMinuteTimer == 1) // 轻载连续累计到20分钟，执行一次 FCC 学习检查
    {
		bTwentyMinuteTimer = 0; // 消费到期标志

	   if( bSocInitializationUpdated == 0 ) // 学习窗口刚开始：记录 OCV 起点并清空窗口吞吐量
	   {
	       gt_app_soc.tsoc_fcc_update.u8SocInit = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table); // 窗口起始 SOC
		   bSocInitializationUpdated = 1; // 后续周期转为评估阶段
		   gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap = 0; // 清除前一窗口吞吐量
		   gt_app_soc.tsoc_fcc_update.dCycleCap = 0; // 清除前一窗口净容量变化
	   }
	   else // 已有学习起点，当前静置样本用于评估容量变化
       {
		   /* Check the differnce between current SOC and initial SOC */
	       s8SocTemporary = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table); // 当前窗口末端 OCV SOC

		   u8SocDifference = abs(s8SocTemporary - gt_app_soc.tsoc_fcc_update.u8SocInit); // 取 SOC 变化绝对值
	       if(u8SocDifference > 40) // SOC跨度大于40个百分点时才估算FCC，降低小区间误差
           {
				 /* Start to recalibrate the FCC */
				 u32CalculatedCapacity = fabs(gt_app_soc.tsoc_fcc_update.dCycleCap) * 100.0 / u8SocDifference; // 将局部吞吐量外推到100% SOC对应容量
				 /* Allow to save into FCC buffer */
				 if ((u32CalculatedCapacity < (gt_app_soc.s32battery_full_cap * 1.05)) // 新FCC必须低于当前FCC的105%
			&&(u32CalculatedCapacity > (gt_app_soc.s32battery_full_cap * 0.90)) // 且高于当前FCC的90%
			&&(u32CalculatedCapacity < MAX_FULLCHG_CAP) // 同时低于工程允许的绝对上限
			&&(u32CalculatedCapacity > MIN_FULLCHG_CAP)) // 且高于工程允许的绝对下限
				{
					/* The first data in the array exits the queue */
					memcpy((unsigned char *)gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf,
						   (unsigned char *)&gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf[1],
						   8); // 移动4个uint16_t，共8字节；丢弃最旧值并为新值腾出末尾槽位
					gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf[4] = u32CalculatedCapacity; // 最新FCC估值入队
					/* FCC calculation param. */
					for (int s32CapacityIndex = 0; s32CapacityIndex < 5; s32CapacityIndex++) // 汇总5次历史学习值以降低单次误差
					{
							 s32CapacitySum += gt_app_soc.tsoc_fcc_update.u16FullChgCapBuf[s32CapacityIndex]; // 累加当前缓存项
					}

					gt_app_soc.s32battery_full_cap = s32CapacitySum / 5; // 取均值更新当前 FCC
				}
				gt_app_soc.tsoc_fcc_update.dCycleCap = 0; // 本轮结束，清空净容量窗口
				gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap = 0; // 本轮结束，清空总吞吐窗口
				gt_app_soc.tsoc_fcc_update.u8SocInit = s8SocTemporary; // 下一轮以当前SOC为起点
	       }
	   }
	}
	else
    {
	   if( gt_app_soc.tsoc_accumulate_cap_data.dChgDsgAccumCap > (gt_app_soc.s32battery_full_cap * 3)) // 吞吐量已超过3倍FCC仍未满足静置学习周期
	   {
		   bSocInitializationUpdated = 0; // 吞吐量超过3倍FCC仍未形成有效SOC跨度，重新建立学习起点
	   }
	}

	u8CurrentSoc = app_soc_voltage_to_soc(u16arv_cellVolt, pu16cell_volt_table); // 当前最高单体对应的 OCV SOC

	if(u8CurrentSoc == 0U) // OCV SOC到达0%端点时丢弃跨区间学习起点
	{
		bSocInitializationUpdated = 0; // 回到0%端点后重新开始FCC学习窗口
	}

	gt_app_soc.u8display_soh = (unsigned char)((double)gt_app_soc.s32battery_full_cap * 100 / SOC_DESIGN_CAP); // 当前FCC/额定设计容量换算 SOH

	if (gt_app_soc.u8display_soh > 100) // 计算结果大于100时按100%封顶
        gt_app_soc.u8display_soh = 100; // FCC高于设计值时SOH封顶100%
	if (gt_app_soc.u32cycle_index < 50) // 学习循环数不足50次时暂不采信FCC衰减结果
        gt_app_soc.u8display_soh = 100; // 循环数不足50次时暂按新电池显示
}

/*
 * 长时间静置后的 OCV 校准。电流处于充放电死区且 SOC 非零时累计静置时间；
 * 达到 30 分钟后按 OCV 表重算真实容量。若偏差超过 FCC 的 0.1%，校正真实容量，
 * 并对显示容量施加相反偏移，尽量避免显示 SOC 突变。
 */
static void SOC_Stand(unsigned short u16arv_cellVolt, const unsigned short *pu16cell_volt_table, unsigned char* u8force_ocv_flag , long s32current)
{
	static unsigned long u32StandSetOcvCount = 0UL; // 静置计时起点，单位为 SOC 250ms tick
	static long s32RealCapacityValue = 0L; // 本次 OCV 查表得到的容量，单位 mAh

	/*静置30分钟查表*/
	 if(1 == app_soc_delay(((s32current > -SOC_STAND_OCV_CURRENT_THRESHOLD) && (s32current < SOC_STAND_OCV_CURRENT_THRESHOLD) && (gt_app_soc.u8display_soc>0)), // 电流位于±200mA静置窗口且SOC未归零时持续计时
																							&u32StandSetOcvCount, // 保存静置计时起点
																							(STAND_SOC_OCV_SET_DELAY_TIME * 4 * 30), // 30分钟换算为250ms tick
																							0, // 静置校准每次触发后重新等待
																							1)) // 静置条件持续到期后置位校准事件
	{
		s32RealCapacityValue = app_soc_voltage_to_capacity(u16arv_cellVolt , pu16cell_volt_table); // 用当前最高单体 OCV 估算静置容量

		if(labs(gt_app_soc.tsoc_capacity_data.s32real_cap_val - s32RealCapacityValue) > (gt_app_soc.s32battery_full_cap/1000)) // OCV与真实容量偏差超过FCC的0.1%时才校正
		{
			gt_app_soc.tsoc_capacity_data.s32display_cap_val -= (gt_app_soc.tsoc_capacity_data.s32real_cap_val - s32RealCapacityValue); // 显示容量反向平移，抵消真实容量校正量
			gt_app_soc.tsoc_capacity_data.s32real_cap_val = s32RealCapacityValue; // 真实容量采用 OCV 结果
		}

		if(gt_app_soc.tsoc_capacity_data.s32display_cap_val < 0) // 防止补偿平移后显示容量落到0以下
		{
			gt_app_soc.tsoc_capacity_data.s32display_cap_val = 0; // 防止反向校正后出现负显示容量
		}

		if(gt_app_soc.tsoc_capacity_data.s32display_cap_val > gt_app_soc.s32battery_full_cap) // 防止校正后显示容量超过FCC
		{
			gt_app_soc.tsoc_capacity_data.s32display_cap_val	= gt_app_soc.s32battery_full_cap; // 显示容量不能超过FCC
		}
		s_app_soc_ocv_update_count++; // 记录一次静置 OCV 校正，供上层任务识别

	}
}

/**
*	@breif CJQ2805
*	必须在ADC采样或AFE获取后取有效值进入该函数
*	该函数采用电流积分计算方式，具有补偿功能，但如电流不准
*   程度太大，可能出现放电欠压时SOC未放完，SOC曲线难看等情况
*	@parameter
*	@u16max_cellVolt: 单节最大电压，不得小于2000mv  单位：mv
*	@u16min_cellVolt: 单节最小电压                 单位：mv
*	@pu16cell_volt_table: OCV table
* 	@s32current: 电流    单位：1mA
*	@au8tim_250ms_flag: 250ms time base      传入1代表250ms时间到，传入后内部清零
*	@u8ucv_flag:  欠压标志位
*	@u8ocv_flag;  过压标志位
*	@au8force_ocv_flag: 强制修改SOC查询OCV表，可用在静置时间很长，或休眠很久唤醒，直接修改。用在产线之类的场景做强制校准
*/
/*
 * 参数说明：max/min_cellVolt 为最高/最低单体电压(mV)，current 为带符号电流(mA)，
 * 充电正、放电负；cell_volt_table 为索引0~100对应SOC 0~100%的升序 OCV 表。
 * tim_250ms_flag=1 表示正常 250ms 积分周期到；min/max_temp 单位℃；ucv/ocv 分别为
 * 欠压/过压标志；force_ocv 非零请求强制 OCV 校准；sleep_flag 表示使用休眠补算节拍。
 * 对外 SOC 单周期入口按顺序执行初始化/必要的 OCV 对齐、容量积分、FCC/SOC 平滑、
 * FCC/SOH 学习、静置校准，最后依据温度/SOC 更新充放电 SOP 限值。
 */
int app_soc_data_handle(unsigned short u16max_cellVolt, unsigned short u16min_cellVolt,
					 const unsigned short *pu16cell_volt_table,
					 long s32current, unsigned char* au8tim_250ms_flag,
					 int s16cell_min_temp,
				     int s16cell_max_temp,
					 unsigned char u8ucv_flag,
					 unsigned char u8ocv_flag,
					 unsigned char* au8force_ocv_flag,
					 unsigned char u8sleep_flag)
{
	if(SOC_Init(u16max_cellVolt, pu16cell_volt_table, au8force_ocv_flag,s32current) != 0) // 无有效电压或初始化失败时终止本周期处理
	{
		return -1; // 电压无效或初始化失败时不继续执行本周期
	}
	SOC_CurrentIntegrator(s32current, au8tim_250ms_flag,u16min_cellVolt, u16max_cellVolt, u8ucv_flag, u8ocv_flag, pu16cell_volt_table, s16cell_min_temp,u8sleep_flag); // 电流积分及带载压降补偿
	SOC_FCCHandle(s32current, u16max_cellVolt,u8ucv_flag, u8ocv_flag,u16min_cellVolt); // 满充/欠压处理及显示SOC平滑
	SOC_FccUpdate(u16max_cellVolt,pu16cell_volt_table,s32current); // 尝试学习FCC并更新SOH
	SOC_Stand(u16max_cellVolt, pu16cell_volt_table, au8force_ocv_flag,s32current); // 检查静置OCV校准条件

	app_sop_task(s16cell_max_temp, s16cell_min_temp, gt_app_soc.u8display_soc, s32current, u16max_cellVolt); // 用最新温度/SOC更新SOP限值
	return 0; // 本周期SOC处理成功
}



/*********************************************************工具类目**********************************************************/

/*
 * 单体电压转 SOC 百分比。要求 OCV 表按 SOC 从低到高升序排列；二分查找返回
 * 相邻表项的低索引作为整数 SOC，不进行表项间插值。电压超出表范围时收敛到端点。
 */
unsigned char app_soc_voltage_to_soc(unsigned short u16bat_val,const unsigned short *pu16cell_volt_table)
{

	unsigned char low = 0; // 当前搜索区间左边界
	unsigned char high = 101; // 左闭右开区间的右边界，表长为101
	unsigned char mid; // 本轮二分查找中点
	while(low < high) // 持续折半搜索，直到命中表项或缩小到相邻表项
	{
		mid = (low + high)/2; // 计算中点索引

		if(u16bat_val == pu16cell_volt_table[mid]) // 与表项完全相等时直接使用该索引作为SOC
		{break;} // 结束二分搜索并返回命中的表项索引
		if(high - low == 1) // 搜索区间只剩相邻表项，返回较低SOC索引
		{
			mid = low; // 不在两个表项之间做百分比插值
			break;
		}
		if(u16bat_val < pu16cell_volt_table[mid]) // 电压较低，继续搜索左半区间
		{ high = mid;}
		else
		{low = mid;} // 电压较高，继续搜索右半区间
	}

	return mid; // OCV表下标即整数SOC百分数
}

/*
 * E200写SOC前的一致性检查：
 * 1. 请求值只允许0~100%；
 * 2. 最高单体电压必须达到SOC_Init使用的最低有效电压；
 * 3. 将最高单体电压按项目OCV表换算成SOC，主机请求值与查表值相差最多5个百分点。
 */
uint8_t app_soc_can_write_matches_table(uint8_t requested_soc,
                                        uint16_t max_cell_voltage_mv,
                                        const uint16_t *cell_voltage_table)
{
	uint8_t table_soc; // 最高单体电压按 OCV 表换算出的SOC
	uint8_t difference; // 主机请求SOC与查表SOC的绝对偏差，单位百分点

	if ((requested_soc > 100U) || (cell_voltage_table == 0) || // 请求超范围或OCV表指针为空
		(max_cell_voltage_mv < 2000U)) // 最高单体电压低于SOC有效采样门限
	{
		return 0U; // SOC越界、表指针为空或电压低于有效下限时拒绝写入
	}

	table_soc = app_soc_voltage_to_soc(max_cell_voltage_mv, cell_voltage_table); // 用最高单体电压得到参考SOC
	difference = (requested_soc >= table_soc) ? // 根据大小关系选择相减顺序，避免无符号下溢
		(uint8_t)(requested_soc - table_soc) : // 请求值不低于表值时直接相减
		(uint8_t)(table_soc - requested_soc); // 请求值低于表值时反向相减

	return (difference <= 5U) ? 1U : 0U; // 偏差不超过5个百分点才通过一致性检查
}

/*
 * 将通过OCV一致性检查的主机SOC写入算法运行状态。
 * 容量使用向上取整，保证后续 remain_cap * 100 / full_cap 仍显示主机请求的整数SOC。
 */
uint8_t app_soc_set_requested_percent(uint8_t requested_soc)
{
	long requested_capacity; // 请求百分比换算出的剩余容量，单位 mAh

	if (requested_soc > 100U) // 只接受0~100%的SOC请求
	{
		return 0U; // 百分比的有效范围是0~100
	}

	if (gt_app_soc.s32battery_full_cap <= 0L) // FCC无效时无法把SOC百分比换算成容量
	{
		return 0U; // FCC尚未初始化时不能可靠地把SOC换算成容量
	}

	requested_capacity = (gt_app_soc.s32battery_full_cap * (long)requested_soc + 99L) / 100L; // 向上取整，确保回算SOC不低于请求整数值
	if (requested_soc == 100U) // 满SOC单独精确映射到完整FCC
	{
		requested_capacity = gt_app_soc.s32battery_full_cap; // 100%明确映射到完整FCC
	}

	gt_app_soc.u8display_soc = requested_soc; // 更新外部可见SOC
	gt_app_soc.s32remain_cap = requested_capacity; // 更新显示剩余容量
	gt_app_soc.tsoc_capacity_data.s32display_cap_val = requested_capacity; // 显示积分容量同步
	gt_app_soc.tsoc_capacity_data.s32real_cap_val = requested_capacity; // 真实积分容量同步
	gt_app_soc.tsoc_capacity_data.s32cap_cnt = 0L; // 清除旧SOC对应的分数容量余数
	gt_app_soc.tsoc_fcc_update.u8SocInit = requested_soc; // FCC学习窗口起点SOC同步

	return 1U; // 主机请求已写入全部相关运行状态
}


unsigned int app_soc_voltage_to_capacity(unsigned short  u16bat_val, const unsigned short *pu16cell_volt_table)
{
	/* 在升序 OCV 表中定位相邻电压项，按区间内的线性比例插值剩余容量；低于表底
	 * 返回 0mAh，高于表顶返回 FCC，端点分支也避免访问表外的 mid+1。 */
	unsigned char low = 0; // 当前二分搜索左边界
	unsigned char high = 101; // 右开边界，OCV表共101项
	unsigned char mid; // 当前搜索中点
	unsigned long  re_value = 0; // 最终容量结果，单位 mAh

	while(low < high) // 二分定位OCV表中的相邻电压点
	{
		mid = (low + high) >> 1; // 每轮将待搜索区间折半
		if(u16bat_val == *(pu16cell_volt_table + mid)) // 与表项完全相等时停止搜索
		{break;} // 精确命中表项时停止搜索
		if(high - low == 1) // 区间只剩相邻SOC表项时，low为插值区间左端
		{
			mid = low; // 剩下相邻表项，low为电压所在区间的下端SOC
			break;
		}
		if(u16bat_val < *(pu16cell_volt_table + mid)) // 输入电压低于中点电压时搜索左侧
		{ high = mid;} // 输入电压较低，保留左侧候选区间
		else
		{low = mid;} // 输入电压较高，保留右侧候选区间
	}

	if(mid == 100) // 命中表顶或更高区域，容量取满FCC
	{
		re_value = gt_app_soc.s32battery_full_cap; // 表顶及以上电压对应100% FCC
	}
	else if(mid == 0)
	{
		re_value = 0; // 表底及以下电压对应0mAh
	}
	else
	{

		/* 先计算 low 索引对应的整百分比容量，再叠加当前电压区间内的线性插值。 */
		re_value = (unsigned long )mid * gt_app_soc.s32battery_full_cap / 100 \
		+ gt_app_soc.s32battery_full_cap * \
			(unsigned long)(u16bat_val - *(pu16cell_volt_table+mid)) / ((*(pu16cell_volt_table+mid+1) - *(pu16cell_volt_table+mid)) * 100); // 叠加当前电压区间内的线性插值容量

	}

	return re_value; // 返回插值后的剩余容量，单位 mAh
}


unsigned char app_soc_delay(unsigned char jude_stat,unsigned long* u32time_base,unsigned long u32delay_time , unsigned char setflag, unsigned char targetflag)
{
	/* 以 SOC 的 250ms 计数器为时基：judge 为假时重置起点；judge 持续为真且累计
	 * 到 delay（单位 tick）后返回 targetflag 并重置起点；状态已等于目标值时也重置计时。 */
	unsigned char bflag = setflag; // 默认保持传入状态，只有条件持续到时才切换到目标状态

	if(setflag == targetflag) // 当前输出已处于目标状态时不继承旧的等待时间
	{
		*u32time_base = gt_app_soc.u32tim_250ms_cnt; // 已处于目标状态时重新起算，避免继承旧的计时长度
	}


	if(jude_stat) // 条件成立，检查从起点到当前时刻的 tick 数
	{
		if((gt_app_soc.u32tim_250ms_cnt - *u32time_base) >=  u32delay_time) // 已累计到配置tick数，状态切换条件成立
		{
			*u32time_base = gt_app_soc.u32tim_250ms_cnt; // 到期后重置起点，后续可按周期再次触发
			bflag = targetflag; // 条件保持足够久，输出目标状态
		}
	}
	else
	{
		*u32time_base = gt_app_soc.u32tim_250ms_cnt; // 条件中断，连续计时清零
	}

	return bflag; // 返回保持后的状态或本次到期设置的目标状态
}


/* 供BMS任务判断本轮SOC计算是否刚执行过OCV表校正。 */
uint32_t app_soc_ocv_update_count(void)
{
	return s_app_soc_ocv_update_count; // 返回初始化/静置校准累计次数，不清零计数器
}
