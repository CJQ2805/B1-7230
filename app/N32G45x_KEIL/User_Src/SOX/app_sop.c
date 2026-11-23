/**
 *  app_sop.c
 *  @Author: CJQ2805
 *  2022/12/27  10:56
 */
#include "app_sop.h"
#include "app_soc.h"
#include "App/app_parallel_id.h"
#include "App/app_param.h"
struct  _app_sop_t   gt_app_sop;

/* SOP温度配置，单位℃。各表的温度边界必须从低到高排列；修改边界不会修改表内电流。 */
/* 放电表共10行：MIN为最低有效温度，ROW1~ROW9为对应行的起始温度。 */
#define APP_SOP_DISCHARGE_TEMP_MIN_C   (-25)
#define APP_SOP_DISCHARGE_TEMP_ROW1_C  (-10)
#define APP_SOP_DISCHARGE_TEMP_ROW2_C  0
#define APP_SOP_DISCHARGE_TEMP_ROW3_C  10
#define APP_SOP_DISCHARGE_TEMP_ROW4_C  20
#define APP_SOP_DISCHARGE_TEMP_ROW5_C  30
#define APP_SOP_DISCHARGE_TEMP_ROW6_C  40
#define APP_SOP_DISCHARGE_TEMP_ROW7_C  50
#define APP_SOP_DISCHARGE_TEMP_ROW8_C  55
#define APP_SOP_DISCHARGE_TEMP_ROW9_C  58

/* 充电表共7行：MIN为最低有效温度，ROW1~ROW6为对应行起始温度，MAX为禁止充电温度。 */
#define APP_SOP_CHARGE_TEMP_MIN_C      (-10)
#define APP_SOP_CHARGE_TEMP_ROW1_C     0
#define APP_SOP_CHARGE_TEMP_ROW2_C     10
#define APP_SOP_CHARGE_TEMP_ROW3_C     20
#define APP_SOP_CHARGE_TEMP_ROW4_C     30
#define APP_SOP_CHARGE_TEMP_ROW5_C     40
#define APP_SOP_CHARGE_TEMP_ROW6_C     50
#define APP_SOP_CHARGE_TEMP_MAX_C      55

/* 回充表共2行：MIN为最低有效温度，ROW1为常温行起始温度，MAX为高温禁止回充温度。 */
#define APP_SOP_RECHARGE_TEMP_MIN_C    (-10)
#define APP_SOP_RECHARGE_TEMP_ROW1_C   0
#define APP_SOP_RECHARGE_TEMP_MAX_C    60

/* 最高电芯温度超过该值时，充放电SOP按最高温查表；否则按最低温查表。 */
#define APP_SOP_HIGH_TEMP_C            40

/* 输入温度为int8_t；错误的温度配置在编译时阻止生成固件。 */
#if (APP_SOP_DISCHARGE_TEMP_MIN_C < -128) || (APP_SOP_DISCHARGE_TEMP_ROW9_C > 127) || \
    (APP_SOP_DISCHARGE_TEMP_MIN_C >= APP_SOP_DISCHARGE_TEMP_ROW1_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW1_C >= APP_SOP_DISCHARGE_TEMP_ROW2_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW2_C >= APP_SOP_DISCHARGE_TEMP_ROW3_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW3_C >= APP_SOP_DISCHARGE_TEMP_ROW4_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW4_C >= APP_SOP_DISCHARGE_TEMP_ROW5_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW5_C >= APP_SOP_DISCHARGE_TEMP_ROW6_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW6_C >= APP_SOP_DISCHARGE_TEMP_ROW7_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW7_C >= APP_SOP_DISCHARGE_TEMP_ROW8_C) || \
    (APP_SOP_DISCHARGE_TEMP_ROW8_C >= APP_SOP_DISCHARGE_TEMP_ROW9_C)
#error "Invalid SOP discharge temperature boundaries"
#endif

#if (APP_SOP_CHARGE_TEMP_MIN_C < -128) || (APP_SOP_CHARGE_TEMP_MAX_C > 127) || \
    (APP_SOP_CHARGE_TEMP_MIN_C >= APP_SOP_CHARGE_TEMP_ROW1_C) || \
    (APP_SOP_CHARGE_TEMP_ROW1_C >= APP_SOP_CHARGE_TEMP_ROW2_C) || \
    (APP_SOP_CHARGE_TEMP_ROW2_C >= APP_SOP_CHARGE_TEMP_ROW3_C) || \
    (APP_SOP_CHARGE_TEMP_ROW3_C >= APP_SOP_CHARGE_TEMP_ROW4_C) || \
    (APP_SOP_CHARGE_TEMP_ROW4_C >= APP_SOP_CHARGE_TEMP_ROW5_C) || \
    (APP_SOP_CHARGE_TEMP_ROW5_C >= APP_SOP_CHARGE_TEMP_ROW6_C) || \
    (APP_SOP_CHARGE_TEMP_ROW6_C >= APP_SOP_CHARGE_TEMP_MAX_C)
#error "Invalid SOP charge temperature boundaries"
#endif

#if (APP_SOP_RECHARGE_TEMP_MIN_C < -128) || (APP_SOP_RECHARGE_TEMP_MAX_C > 127) || \
    (APP_SOP_RECHARGE_TEMP_MIN_C >= APP_SOP_RECHARGE_TEMP_ROW1_C) || \
    (APP_SOP_RECHARGE_TEMP_ROW1_C >= APP_SOP_RECHARGE_TEMP_MAX_C)
#error "Invalid SOP recharge temperature boundaries"
#endif

#if (APP_SOP_HIGH_TEMP_C < -128) || (APP_SOP_HIGH_TEMP_C > 127)
#error "Invalid SOP high temperature switch"
#endif

/*
 * 放电 SOP 三维表：依次为 10s/30s/持续档、10个温度区间、11个SOC区间。
 * 图片中的电流单位为A，固件输出单位为0.1A，因此录入值乘10；例如125A写为1250。
 * SOC列按<=5%、>5~10%、>10~20%、>20~30%、>30~40%、>40~50%、>50~60%、
 * >60~70%、>70~80%、>80~90%、>90~100%排列。行注释为默认温度，实际分档由app_param.h配置。
 */
const float	afdischarge_sop_table[3][10][11] = {
	// 10s档
	{//SOC
		{100,100,150,150,180,200,250,250,300,350,400}, // -25℃≤T<-10℃，图片电流×10
		{150,250,300,400,500,600,600,700,800,900,1000}, // -10℃≤T<0℃
		{160,250,400,400,550,700,700,800,900,1000,1100}, // 0℃≤T<10℃
		{170,300,800,700,800,800,800,1000,1100,1100,1300}, // 10℃≤T<20℃
		{180,350,1000,1100,1300,1300,1300,1300,1300,1300,1300}, // 20℃≤T<30℃
		{190,400,1200,1300,1500,1500,1500,1500,1500,1500,1500}, // 30℃≤T<40℃
		{200,500,1200,1300,1500,1500,1500,1500,1500,1500,1500}, // 40℃≤T<50℃
		{150,200,600,1300,1500,1500,1500,1500,1500,1500,1500}, // 50℃≤T<55℃
		{80,50,100,200,200,200,200,200,200,200,200}, // 55℃≤T<58℃
		{50,50,50,50,50,50,50,50,50,50,50} // T≥58℃
	},
	// 30s档
	{
		{50,100,120,150,180,200,250,250,300,350,400}, // -25℃≤T<-10℃
		{120,250,300,400,400,400,500,600,600,900,900}, // -10℃≤T<0℃
		{140,250,400,400,550,700,700,800,900,900,900}, // 0℃≤T<10℃
		{160,300,800,700,800,800,800,900,900,1000,1000}, // 10℃≤T<20℃
		{170,350,900,900,1000,1000,1000,1000,1000,1250,1250}, // 20℃≤T<30℃
		{170,400,900,1250,1250,1250,1250,1250,1250,1250,1250}, // 30℃≤T<40℃
		{200,500,950,1250,1250,1250,1250,1250,1250,1250,1250}, // 40℃≤T<50℃
		{150,200,600,1250,1250,1250,1250,1250,1250,1250,1250}, // 50℃≤T<55℃
		{80,80,100,200,200,200,200,200,200,200,200}, // 55℃≤T<58℃
		{50,50,50,50,50,50,50,50,50,50,50} // T≥58℃
	},
	// 持续档
	{
		{50,100,120,150,180,200,250,250,300,350,400}, // -25℃≤T<-10℃
		{120,250,300,400,400,400,500,600,600,900,900}, // -10℃≤T<0℃
		{140,250,400,400,550,700,700,800,900,900,900}, // 0℃≤T<10℃
		{160,300,800,700,800,800,800,900,900,900,900}, // 10℃≤T<20℃
		{170,350,900,900,900,900,900,900,900,900,900}, // 20℃≤T<30℃
		{170,400,900,900,900,900,900,900,900,900,900}, // 30℃≤T<40℃
		{200,500,900,900,900,900,900,900,900,900,900}, // 40℃≤T<50℃
		{150,200,600,900,900,900,900,900,900,900,900}, // 50℃≤T<55℃
		{80,80,100,200,200,200,200,200,200,200,200}, // 55℃≤T<58℃
		{50,50,50,50,50,50,50,50,50,50,50} // T≥58℃
	}
};


/*
 * 充电 SOP 二维表：7个温度区间×11个SOC区间，表格原单位A，代码单位0.1A，录入时乘10。
 * SOC=0没有单独区间，运行时按最低SOC列处理；行注释为默认温度，实际分档由app_param.h配置。
 */
const float	afcharge_sop_table[7][11] = {
	// -10℃≤T<0℃：0~5%为0.5A，其余SOC区间为1A
	{5,10,10,10,10,10,10,10,10,10,10},
	// 0℃≤T<10℃
	{15,15,90,90,90,90,90,90,60,90,30},
	// 10℃≤T<20℃
	{15,15,180,180,180,180,180,180,90,90,60},
	// 20℃≤T<30℃
	{15,15,300,300,300,300,300,300,210,150,60},
	// 30℃≤T<40℃
	{15,15,300,300,300,300,300,300,210,150,60},
	// 40℃≤T<50℃
	{15,15,120,120,120,120,120,120,120,60,60},
	// 50℃≤T<55℃
	{15,15,15,30,30,30,30,30,30,30,30}
};

/* 反馈 SOP 表：温度范围由app_param.h配置；列分别为SOC<=95%和SOC>95%。单位0.1A。 */
static const uint16_t au16feedback_sop_table[2][2] = {
	{30, 0},   // -10℃≤T<0℃：SOC<=95%限3.0A，否则0A
	{300, 0}   // 0℃≤T<60℃：SOC<=95%限30.0A，否则0A
};

/*
 * 将放电温度和SOC映射到图片中的10行×11列。
 * 温度边界采用左闭右开，避免原先对边界温度只在首次调用时预置、运行中可能沿用旧档的问题。
 */
void SOP_Get_DischargeTempSOC_Level(uint8_t u8soc, int8_t s8temp)
{
	/* 选择SOC列；SOC=0也使用最低SOC列，避免没有对应表列。 */
	if(u8soc <= 5U) // 0~5% -> 第0列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 0;
	}
	else if(u8soc <= 10U) // >5~10% -> 第1列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 1;
	}
	else if(u8soc <= 20U) // >10~20% -> 第2列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 2;
	}
	else if(u8soc <= 30U) // >20~30% -> 第3列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 3;
	}
	else if(u8soc <= 40U) // >30~40% -> 第4列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 4;
	}
	else if(u8soc <= 50U) // >40~50% -> 第5列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 5;
	}
	else if(u8soc <= 60U) // >50~60% -> 第6列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 6;
	}
	else if(u8soc <= 70U) // >60~70% -> 第7列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 7;
	}
	else if(u8soc <= 80U) // >70~80% -> 第8列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 8;
	}
	else if(u8soc <= 90U) // >80~90% -> 第9列
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 9;
	}
	else // >90%，包括异常的大于100值，按最高SOC列限值处理
	{
		gt_app_sop.tbms_discharge_level.u8soc_level = 10;
	}

	/* 选择温度行；有效温度下限由调用方检查。 */
	if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW1_C) // 第0行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 0;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW2_C) // 第1行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 1;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW3_C) // 第2行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 2;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW4_C) // 第3行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 3;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW5_C) // 第4行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 4;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW6_C) // 第5行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 5;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW7_C) // 第6行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 6;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW8_C) // 第7行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 7;
	}
	else if(s8temp < APP_SOP_DISCHARGE_TEMP_ROW9_C) // 第8行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 8;
	}
	else // 第9行
	{
		gt_app_sop.tbms_discharge_level.u8temp_level = 9;
	}
}



/*
 * 按当前温度/SOC 查出放电 SOP，并根据实测放电电流和持续时间切换 10s/30s/持续档。
 * 输入电流采用项目符号（放电为负），函数内取反后与正值放电能力比较；返回值单位 0.1A。
 */
int16_t GetDischargeSOPLevel(int16_t s16current, uint8_t u8soc,  int8_t s8temp)
{
	int16_t s16sop_current = 0, s16arr_current = 0; // 当前返回限值及转换状态时比较用的限值，单位0.1A
	static unsigned long dlyTimeUpgrade30S = 0, dlyTimeUpgradeCont = 0; // 升档计时起点，单位为250ms tick
	static unsigned long dlyTimeRecover30S = 0, dlyTimeRecoverCont = 0; // 降档计时起点，单位为250ms tick

	/* 低于表格最低温度时不套用最低温行，直接返回0并清掉档位计时。 */
	if(s8temp < APP_SOP_DISCHARGE_TEMP_MIN_C)
	{
		dlyTimeUpgrade30S = gt_app_soc.u32tim_250ms_cnt;
		dlyTimeUpgradeCont = gt_app_soc.u32tim_250ms_cnt;
		dlyTimeRecover30S = gt_app_soc.u32tim_250ms_cnt;
		dlyTimeRecoverCont = gt_app_soc.u32tim_250ms_cnt;
		return 0;
	}

	/* 将负值放电电流转换成正的放电电流幅值，以便与表中的正数限值比较。 */
	s16current = -1*s16current; // 放电电流的项目符号为负，取反后转为正幅值参与限值比较
	SOP_Get_DischargeTempSOC_Level(u8soc,s8temp); // 更新本次查表所用的温度/SOC下标
	/* 先按当前持续时间档和本次温度/SOC 下标读取输出值；状态变化在本次查表后影响后续周期。 */
	s16sop_current = afdischarge_sop_table[gt_app_sop.tbms_discharge_level.u8cur_level][gt_app_sop.tbms_discharge_level.u8temp_level][gt_app_sop.tbms_discharge_level.u8soc_level]; // 依当前档位读取待返回限值

	s16arr_current = afdischarge_sop_table[Cur_LEVEL_10S][gt_app_sop.tbms_discharge_level.u8temp_level][gt_app_sop.tbms_discharge_level.u8soc_level]; // 读取10秒档限值，作为进入30秒档的门限

	/* 当前为10秒档且电流超过10秒档限值连续5秒后，升级到30秒档。 */
	if(1 == app_soc_delay((gt_app_sop.tbms_discharge_level.u8cur_level == Cur_LEVEL_10S) && (s16current > s16arr_current), // 仅10秒档且电流超限时持续计时
			&dlyTimeUpgrade30S, // 保存10秒档升级判据的计时起点
			5 * 4, // 5秒换算为250ms tick
		        0, // 每次重新检查时从未到期状态开始
		        1)) // 条件持续到期后置位升级事件
	{
		gt_app_sop.tbms_discharge_level.u8cur_level = Cur_LEVEL_30S; // 超过10秒能力后进入30秒档
	}

	/* 取相同温度/SOC下的30秒档限值；超限连续25秒后进入持续档。 */
	s16arr_current = afdischarge_sop_table[Cur_LEVEL_30S][gt_app_sop.tbms_discharge_level.u8temp_level][gt_app_sop.tbms_discharge_level.u8soc_level]; // 读取30秒档限值
	if(1 == app_soc_delay((gt_app_sop.tbms_discharge_level.u8cur_level == Cur_LEVEL_30S) && (s16current > s16arr_current), // 仅30秒档且电流超过30秒限值时持续计时
			&dlyTimeUpgradeCont, // 保存进入持续档的计时起点
			25 * 4, // 25秒换算为250ms tick
		        0, // 初始状态为未到期
		        1)) // 条件持续到期后置位进入持续档事件
	{
		gt_app_sop.tbms_discharge_level.u8cur_level = Cur_LEVEL_Cont; // 超过30秒能力后切换到持续档
	}


	/* 两个恢复计时器每轮都更新；离开对应档位或恢复条件中断时会清零连续计时。 */
	s16arr_current = afdischarge_sop_table[Cur_LEVEL_30S][gt_app_sop.tbms_discharge_level.u8temp_level][gt_app_sop.tbms_discharge_level.u8soc_level]; // 读取30秒档恢复门限
	if(1 == app_soc_delay((gt_app_sop.tbms_discharge_level.u8cur_level == Cur_LEVEL_30S) && (s16current < s16arr_current - 5), // 30秒档且电流低于限值0.5A以上才开始计时
			&dlyTimeRecover30S, // 保存30秒档恢复计时起点
			5 * 4, // 需要连续满足5秒
	        0, // 初始状态为未到期
	        1)) // 到期后置位恢复事件
	{
		gt_app_sop.tbms_discharge_level.u8cur_level = Cur_LEVEL_10S; // 30秒档电流回落并稳定后退回10秒档
	}

	s16arr_current = afdischarge_sop_table[Cur_LEVEL_Cont][gt_app_sop.tbms_discharge_level.u8temp_level][gt_app_sop.tbms_discharge_level.u8soc_level]; // 读取持续档恢复门限
	if(1 == app_soc_delay((gt_app_sop.tbms_discharge_level.u8cur_level == Cur_LEVEL_Cont) && (s16current < s16arr_current - 5), // 持续档且电流低于限值0.5A以上才开始计时
			&dlyTimeRecoverCont, // 保存持续档恢复计时起点
			5 * 4, // 需要连续满足5秒
	        0, // 初始状态为未到期
	        1)) // 到期后置位恢复事件
	{
		gt_app_sop.tbms_discharge_level.u8cur_level = Cur_LEVEL_30S; // 持续档电流回落并稳定后退回30秒档
	}

	return s16sop_current; // 返回函数开始时当前档位查出的限流值，本次档位切换从下周期生效
}


/*
 * 把温度和SOC映射到充电表的7行×11列。
 * 该函数只在有效温度范围内调用；范围外温度由 GetChargeSOPLevel() 直接限为0A。
 */
void SOP_Get_ChargeTempSOC_Level(uint8_t u8soc, int8_t s8temp)
{
	/* SOC=0虽不在图片的0%<SOC区间定义内，仍按最低SOC列保守处理。 */
	if(u8soc <= 5U) // 0~5% -> 第0列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 0;
	}
	else if(u8soc <= 10U) // >5~10% -> 第1列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 1;
	}
	else if(u8soc <= 20U) // >10~20% -> 第2列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 2;
	}
	else if(u8soc <= 30U) // >20~30% -> 第3列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 3;
	}
	else if(u8soc <= 40U) // >30~40% -> 第4列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 4;
	}
	else if(u8soc <= 50U) // >40~50% -> 第5列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 5;
	}
	else if(u8soc <= 60U) // >50~60% -> 第6列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 6;
	}
	else if(u8soc <= 70U) // >60~70% -> 第7列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 7;
	}
	else if(u8soc <= 80U) // >70~80% -> 第8列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 8;
	}
	else if(u8soc <= 90U) // >80~90% -> 第9列
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 9;
	}
	else // >90% -> 第10列，包含异常的SOC>100%
	{
		gt_app_sop.tbms_charge_level.u8soc_level = 10;
	}

	/* 温度采用左闭右开区间；最低和最高温度由调用方检查。 */
	if(s8temp < APP_SOP_CHARGE_TEMP_ROW1_C) // 第0行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 0;
	}
	else if(s8temp < APP_SOP_CHARGE_TEMP_ROW2_C) // 第1行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 1;
	}
	else if(s8temp < APP_SOP_CHARGE_TEMP_ROW3_C) // 第2行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 2;
	}
	else if(s8temp < APP_SOP_CHARGE_TEMP_ROW4_C) // 第3行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 3;
	}
	else if(s8temp < APP_SOP_CHARGE_TEMP_ROW5_C) // 第4行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 4;
	}
	else if(s8temp < APP_SOP_CHARGE_TEMP_ROW6_C) // 第5行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 5;
	}
	else // 第6行
	{
		gt_app_sop.tbms_charge_level.u8temp_level = 6;
	}
}

int16_t GetChargeSOPLevel(uint8_t u8soc, int8_t s8temp)
{
	int16_t s16sop_current = 0; // 查表得到的充电 SOP 限值，单位0.1A
	if((s8temp < APP_SOP_CHARGE_TEMP_MIN_C) || (s8temp >= APP_SOP_CHARGE_TEMP_MAX_C))
	{
		return 0;
	}

	/* 先更新温度/SOC 下标，再返回对应充电电流能力，单位 0.1A。 */
	SOP_Get_ChargeTempSOC_Level(u8soc,s8temp); // 将输入参数转为表格行列索引
	s16sop_current = afcharge_sop_table[gt_app_sop.tbms_charge_level.u8temp_level][gt_app_sop.tbms_charge_level.u8soc_level]; // 读取对应温度/SOC限流值

	return s16sop_current; // 返回最大允许充电电流，单位0.1A
}


/*
 * 对外发布最大放电电流时做斜率限制：每经过500ms，向目标限值移动1.0A；
 * 最后一步贴齐目标值，避免按整安步进越过0.1A精度的查表目标。
 */
void SOP_GetDischargeSopCurrent(int16_t s16current)
{
	static uint8_t su8flag = 0; // 记录输出限值是否已用首个目标值初始化
	static unsigned long dlyflag = 0; // 输出斜率计时起点，单位为 SOC 的 250ms tick

	/* 目标值和当前输出使用同一 0.1A 单位；该自赋值是原有空操作，不改变数值。 */
	s16current = s16current; // 保留原有语句，不对传入目标作单位换算
	if(su8flag == 0) // 首次调用时用第一个目标限值初始化对外放电限值
	{
		gt_app_sop.s16max_discharge_current01A = s16current; // 首次调用直接以当前SOP限值初始化输出
		su8flag = 1; // 后续周期改用逐档斜率调整
	}

	if(gt_app_sop.s16max_discharge_current01A > s16current) // 当前输出高于目标，按斜率逐步收紧
	{
		/* 目标限值变小，按1.0A步进收紧，并在目标附近精确贴齐。 */
		if(1 == app_soc_delay(1, // judge恒为真，启动一次输出斜率等待
		 &dlyflag, // 保存当前斜率调整计时起点
		 BMS_CUR_RATE_OF_CHANGE_INTERVAL_TICKS, // 等待500ms（2个250ms tick）
		 0, // 当前尚未发生本次斜率步进
		 1)) // 到期后允许本次调整
		{
			if((gt_app_sop.s16max_discharge_current01A - s16current) > BMS_CUR_RATE_OF_CHANGE_STEP_01A)
			{
				gt_app_sop.s16max_discharge_current01A -= BMS_CUR_RATE_OF_CHANGE_STEP_01A;
			}
			else
			{
				gt_app_sop.s16max_discharge_current01A = s16current;
			}
		}
	}
	else if(gt_app_sop.s16max_discharge_current01A < s16current) // 当前输出低于目标，按斜率逐步放宽
	{
		/* 目标限值变大时也按1.0A步进恢复，最后一步精确贴齐目标。 */
		if(1 == app_soc_delay(1, // judge恒为真，启动一次输出斜率等待
		 &dlyflag, // 复用斜率计时器记录本次步进起点
		 BMS_CUR_RATE_OF_CHANGE_INTERVAL_TICKS, // 等待500ms（2个250ms tick）
		 0, // 当前尚未发生本次斜率步进
		 1)) // 到期后允许本次调整
		{
			if((s16current - gt_app_sop.s16max_discharge_current01A) > BMS_CUR_RATE_OF_CHANGE_STEP_01A)
			{
				gt_app_sop.s16max_discharge_current01A += BMS_CUR_RATE_OF_CHANGE_STEP_01A;
			}
			else
			{
				gt_app_sop.s16max_discharge_current01A = s16current;
			}
		}
	}
}

/* 按反馈 SOP 表计算回充限值；温度超限或SOC>95%时置0。电流单位为0.1A。 */
void SOP_GetRechargeSOPCurrent(uint8_t u8soc, int8_t s8max_temp, int8_t s8min_temp)
{
	uint8_t u8temp_level;
	uint8_t u8soc_level;

	/* 温度超出配置范围时禁止回充。 */
	if((s8min_temp < APP_SOP_RECHARGE_TEMP_MIN_C) ||
	   (s8max_temp >= APP_SOP_RECHARGE_TEMP_MAX_C))
	{
		gt_app_sop.u16max_recharge_current01A = 0;
		return;
	}
	u8temp_level = (s8min_temp < APP_SOP_RECHARGE_TEMP_ROW1_C) ? 0U : 1U;

	u8soc_level = (u8soc <= 95U) ? 0U : 1U;
	gt_app_sop.u16max_recharge_current01A = au16feedback_sop_table[u8temp_level][u8soc_level];
}


void SOP_GetChargeSopCurrent(int16_t s16current)
{
	/* 充电 SOP 当前直接发布查表结果，没有像放电输出那样做斜率平滑。 */
	gt_app_sop.s16max_charge_current01A = s16current; // 保存最大充电电流，单位0.1A
}

/* 每个 SOC 计算周期的 SOP 总调度：选择评估温度，更新放电、充电和回充电流限值。 */
void app_sop_task(int8_t s8max_temp, int8_t s8min_temp, uint8_t u8soc, int32_t s32current)
{
	uint8_t u8soc_temp = 0; // 本周期使用的SOC查表值，单位%
	int8_t s8temp = 0; // 根据高温分界选择出的SOP评估温度，单位℃
	int16_t s16sopcurrent = 0, s16discharge_current = 0; // 充/放电SOP中间值，单位0.1A

	/* 极热时用最高温度限制能力；其余情况用最低温度，优先反映最弱电芯。 */
	if(s8max_temp > APP_SOP_HIGH_TEMP_C) // 最高温超过高温分界时，以最高温作为限值查表温度
	{
		s8temp = s8max_temp; // 高温保护侧优先使用最高温度
	}
	else
	{
		s8temp = s8min_temp; // 其他温区使用最低温度反映较弱电芯
	}

	u8soc_temp = u8soc; // 保留SOC输入供充放电查表使用

	/* 放电能力先按持续时间/温度/SOC 查表。低于表格下限时直接归零，不走斜率缓降。 */
	s16discharge_current = GetDischargeSOPLevel(s32current, u8soc_temp, s8temp); // 获取当前放电持续时间档的允许电流
	if(s8temp < APP_SOP_DISCHARGE_TEMP_MIN_C) // 低于配置下限时，放电限值立即归零
	{
		gt_app_sop.s16max_discharge_current01A = 0; // 直接发布0A，不等待斜率调节周期
	}
	else
	{
		SOP_GetDischargeSopCurrent(s16discharge_current); // 对有效温度范围内的放电限值做逐0.1A斜率平滑
	}

	/* 充电能力按温度和 SOC 查表，直接发布查表电流。 */
	s16sopcurrent = GetChargeSOPLevel(u8soc_temp, s8temp); // 获取当前温度/SOC档的允许充电电流
	SOP_GetChargeSopCurrent(s16sopcurrent); // 直接发布充电查表限值

	SOP_GetRechargeSOPCurrent(u8soc_temp, s8max_temp, s8min_temp); // 按温度和SOC计算回充能力

	/* 并联运行时，放电SOP按本机压差和放电故障门控；充电及回充SOP不使用该并联门控。 */
	if ((app_parallel_id_is_enabled() != 0U) &&
		(app_parallel_id_is_ready() != 0U) &&
		(app_parallel_cmd_en() == 0U))
	{
		gt_app_sop.s16max_discharge_current01A = 0;
	}
}
