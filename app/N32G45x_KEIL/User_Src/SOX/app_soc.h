/*
 *  app_soc.h
 *  Author: CJQ2805
 *  SOC宏定义函数 
 *	这里用户需要做的是修改各类宏参数来适配自己的项目
 *  The file comments use UTF-8 encoding.
 *	2022/11/16    10:31:11
 *    
 *  使用说明：
 *  用户导入模块后，
 *  ①根据自己的项目修改/用户修改的宏/中的内容，其中最重要修改的参数为：电池充放电平均内阻（该算法最重要的参数，计算方式详见阻抗SOC算法内阻计算方式）、电池总容量，其他参数根据需求修改
 *  ②/快充满时的补偿点/涉及到充电曲线的情况，电流精度OK的情况下，基本不需要修改
 *  ③/快放空时的补偿点/该项参数的欠压值非常重要，欠压值+100mV即为SOC快速收敛的电压值，SOC允许归0的电压值根据需求设定。CC和CV模式充电末端加速的方案不同，根据实际情况选择。
 *  ④/OCV参数设置/这里设置静置多久会去查表跟随，和具体时间，默认是1S(4*250ms)。
 *  ⑤/FCC判断阈值/ CC模式只有电压判断， CV模式填写相应的满充条件，条件满足后，会置位满充标志位 u8soc_fcc_flag = 1
 *  ⑥/FCC解除判断阈值/ 填写相应满充解除条件，会清除满充标志位 u8soc_fcc_flag = 0
 *  结构体参数，有标注显示用的参数，都是可以直接调用结构体供外部使用。
 *
 */ 

#ifndef __APP_SOC_H
#define __APP_SOC_H
#include "stdint.h"
#include "App/app_param.h"

/**************************用户修改的宏********************************/
//充电为正，放电为负						  
#define SOC_DESIGN_CAP						  APP_PARAM_BAT_CAPACITY //mAh 电池额定设计容量
#define SOC_CURRENT_SELF_CONSUME    		  7        		         //mA   板子自耗电
#define SOC_OFFSET_THRESHOLD   				  10		   	 	     //%    允许直接修改的SOC阈值(查表值和当前值差距大于该值直接修改SOC)
#define SOC_CAP_OFFSET_THRESHOLD    		  SOC_DESIGN_CAP*0.1     //1mAh 允许直接修改的容量阈值（同上，容量差距太大直接修改SOC）
#define SOC_CHAG_CURRENT_HANDLE     		  80		    	     //mA   允许计算SOC的充电电流
#define SOC_DISCHRG_CHRRCENT_HANDLE 		  -80	        	     //mA   允许计算SOC的放电电流
#define SOC_HUNDREDTH_CAP           		  SOC_DESIGN_CAP*36      //mA   百分之一的容量所产生的电流
#define SOC_COMPENSATE_RESISTANCE     		  14				     //mR   电池包充放电平均内阻
#define SOC_HOW_LONG_SECOND_ALLOW_ADD_ONE     1             	     //S
/*
快充满时的补偿点
The compensation point when it's nearly full
*/
#define SOC_CHAG_HIGH_SPEED_VOLTAGE_SOC       95			         //%	   该值不建议修改
#define SOC_CHAG_HIGH_SPEED_PERCENT_SECOND    5                      //S    Up to the above Voltage and Current conditon,5S soc add 1%。

/*
快放空时的补偿点
The compensation point of the fast air
*/
#define SOC_MAX_CUR_FILTER_LIMIT_VUL		   SOC_HUNDREDTH_CAP/SOC_CAP_CNT_VAL + 9  //SOC_HUNDREDTH_CAP/SOC_CAP_CNT_VAL + 最大放电电流/SOC_CAP_CNT_VAL

#define SOC_DSG_CUV_VOLTAGE				   2700        		         // 欠压值
#define SOC_DSG_SOC_RETURN_0               2900        		         // SOC允许归0值
#define SOC_DSG_HIGH_SPEED_VOLTAGE_SOC     5			             //% 该值不建议修改

#define FCC_SET_MODE             			   1                     //0:CC充电模式     1:CV充电模式
#define SOC_FCC_SET_DELAY_TIME			       5		             //S FCC的阈值时间，达到该电压和小于该电流时，持续该时间达满充

#define SOC_DISCHG_KEEP_ONE_VOLTAGE        3000
/*
静置时ocv参数设置
*/
#define STAND_SOC_OCV_SET_DELAY_TIME		   60		             //1MIN
#define SOC_STAND_OCV_CURRENT_THRESHOLD     200L                     //mA          静置OCV校准允许的电流幅值
#define STAND_SOC_SLEEP_DELAY_TIME		       4		             //250ms	   休眠所需时间

/******************************************CC充电模式参数设置***********************************************************/
#define SOC_FCC_SET_VOLTAGE                    3600
/******************************************CC充电模式参数设置***********************************************************/

/******************************************CV充电模式参数设置***********************************************************/
/*
FCC判断阈值 CV模式
The FCC determines the threshold
*/
#define SOC_FCC_ACCELERATE_CONDITION			(((s32current > S0C_FCC_SET_STOP_CURRENT_MIN)&& (s32current < SOC_FCC_SET_STOP_CURRENT_MAX) && (u32cell_arv_volt > SOC_FCC_SET_MIN_CELL_VOLTAGE)) )
#define SOC_FCC_SET_MIN_CELL_VOLTAGE            4000                 //mV   FCC的阈值电压，达到该电压和小于该电流时，持续该时间达满充
#define SOC_FCC_SET_STOP_CURRENT_MAX		    500                  //mA   FCC的阈值电流，达到该电压和小于该电流时，持续该时间达满充
#define S0C_FCC_SET_STOP_CURRENT_MIN            80		             //mA   FCC电流下限值，认为有充电的值。
/******************************************CV充电模式参数设置***********************************************************/
/*
FCC解除判断阈值
FCC release judgment threshold
*/
#define SOC_FCC_RESET_MIN_CELL_VOLTAGE          3900                 //mV   FCC电压如果达到该值时，持续该时间，解除FCC
#define SOC_FCC_RESET_CURRENT     		        -500                 //mA   有该大小放电电流，持续该时间，解除FCC
#define SOC_FCC_RESET_DELAY_TIME                1                    //S    持续时间

/************************************************************************************************************************/

/******************************************FCCUpate模式参数设置**********************************************************/
/*
FCC使用的设计容量衰减
*/
#define MAX_FULLCHG_CAP		SOC_DESIGN_CAP 
#define MIN_FULLCHG_CAP		SOC_DESIGN_CAP *0.7

/************************************************************************************************************************/

#define SOC_CAP_CNT_VAL 			             14400   	         //1mah  3600 * 4  250MS

struct _soc_accumulate_capacity_data_t{

	double s32total_accumulate_charge_cap;
	double s32total_accumulate_discharge_cap;
	double s32single_accumulate_charge_cap;
	double s32single_accumulate_discharge_cap;
	double dChgDsgAccumCap;
	
};

struct _soc_compensate_para_t{
	
	char s8stock_soc;
	char s8real_soc;
	
};

struct _soc_capacity_data_t{

	long s32real_cap_val;
	long s32display_cap_val;
	long s32cap_cnt;
};

struct _soc_fcc_data_t{

	unsigned char u8handle_soc;
	unsigned char u8fcc_condition_flag;

};

struct _soc_fcc_update_t{

	//此处开始是FCCUpdate的部分内容
	unsigned char   u8SocInit;
	unsigned short	u16FullChgCapBuf[5];		//FCC容量存储值		
	double    		dCycleCap;					//CycleCap，本次循环总容量
};




struct _app_soc_t{

	struct _soc_accumulate_capacity_data_t          tsoc_accumulate_cap_data;   //累积容量   显示用
	struct _soc_compensate_para_t 					tsoc_compensate_para;		//内部使用
  	struct _soc_capacity_data_t 					tsoc_capacity_data; 		//mah 内部使用
  	struct _soc_fcc_data_t	  					    tsoc_fcc_data;       		//内部使用 
	struct _soc_fcc_update_t						tsoc_fcc_update;			//FCCUpdate的参数
	
  	long s32battery_full_cap;		  				   							//FCC mAH 满充容量   显示用
  	unsigned char	u8display_soc;   											//SOC 显示用SOC 第34字节
	unsigned char	u8display_soh;												//SOH 显示用SOH
	unsigned char	u8soc_fcc_flag;												//满充标志位  显示用
  	long	s32remain_cap;					  									//remain_capacity 剩余容量，显示用
	unsigned long	u32cycle_index;          									//循环次数  cycle  显示用
	unsigned long	u32tim_250ms_cnt;											//time base

};

extern struct _app_soc_t  gt_app_soc;



unsigned char app_soc_voltage_to_soc(unsigned short u16bat_val,const unsigned short *pu16cell_volt_table);
/* 判断主机写入SOC与当前最高单体电压OCV查表SOC的差值是否不超过5个百分点。 */
uint8_t app_soc_can_write_matches_table(uint8_t requested_soc,
                                        uint16_t max_cell_voltage_mv,
                                        const uint16_t *cell_voltage_table);
/* 将主机请求SOC写入显示SOC及SOC算法使用的剩余容量状态。 */
uint8_t app_soc_set_requested_percent(uint8_t requested_soc);
/* 返回SOC模块发生OCV查表校正的累计序号，用于区分查表校正与普通电流积分。 */
uint32_t app_soc_ocv_update_count(void);
unsigned int  app_soc_voltage_to_capacity(unsigned short u16bat_val,const unsigned short *pu16cell_volt_table);
unsigned char app_soc_delay(unsigned char jude_stat,unsigned long* s32time_base,unsigned long u32delay_time , unsigned char setflag, unsigned char targetflag);
int app_soc_data_handle(unsigned short u16max_cellVolt, unsigned short u16min_cellVolt,
					 const unsigned short *pu16cell_volt_table,
					 long s32current, unsigned char* au8tim_250ms_flag,
					 int s16cell_min_temp, 
				     int s16cell_max_temp,
					 unsigned char u8ucv_flag,
					 unsigned char u8ocv_flag,	 
					 unsigned char* au8force_ocv_flag,
					 unsigned char u8sleep_flag);

					


					
					
					
#endif
