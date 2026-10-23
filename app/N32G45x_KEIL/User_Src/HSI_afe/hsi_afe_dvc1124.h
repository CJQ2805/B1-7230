#ifndef _HSI_AFE_DVC1124_H_
#define _HSI_AFE_DVC1124_H_
//------------------------------------------------------------------------------
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include "App/app_param.h"
#include "Driver/d_gpio.h"
#include "Middle/m_soft_i2c.h"

#define	 AFE_MAX_CELL_CNT               APP_BMS_MAX_CELL_NUM
#define  AFE_MAX_GP_CNT                 6
#define  AFE_REG_VALUE(Rn)              *((uint8_t *)&gt_hsi_afe_dvc1124_regs.Rn)
#define  AFE_ADDR_R(n)                   n
#define _lsbVCELL                       0.0001 //VADC
#define _lsbVCELL_signed                0.0002 //VADC
#define _lsbCC1                        (5e-6)//CC1
#define _lsbCC2                        (0.3125e-6)//CC2
//------------------------------------------------------------------------------	
#define ADDRESS 						0x40//IIC
#define INVALID_TEMPERATURE            -128  //
#define GENERAL_DEBOUNCE_DLY_S          3    //
#define AFE_CMD_RETRY_MAX               3	 //SPI

/* AFE_NTC采样结果：通信链路异常和NTC本身电气异常必须分别表达。 */
typedef enum
{
	HSI_AFE_DVC1124_NTC_STATUS_INVALID = 0U,          /* 尚未完成一次有效采样。 */
	HSI_AFE_DVC1124_NTC_STATUS_VALID = 1U,            /* IIC读取成功且NTC阻值在有效范围内。 */
	HSI_AFE_DVC1124_NTC_STATUS_ELECTRICAL_FAULT = 2U, /* NTC短路、断线或阻值越界。 */
	HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT = 3U         /* AFE寄存器读取失败，归入AFE通信故障。 */
} hsi_afe_dvc1124_ntc_status_t;
//------------------------------------------------------------------------------
#pragma pack (push,1)
enum{GP1=0,GP2,GP3,GP4,GP5,GP6};		//GPn
//------------------------------------------------------------------------------

typedef struct
{
	union
	{
	__attribute__((packed)) struct{
	uint8_t SCD:1;   //REGMODE01/R0[0]/0x00/SCD_FLAG, 放电短路
	uint8_t OCC2:1;  //REGMODE01/R0[1]/0x00/OCIC2_FLAG, 2级充电过流
	uint8_t OCD2:1;  //REGMODE01/R0[2]/0x00/OCID2_FLAG, 2级放电过流
	uint8_t OCC1:1;  //REGMODE01/R0[3]/0x00/OCIC1_FLAG, 1级充电过流
	uint8_t OCD1:1;  //REGMODE01/R0[4]/0x00/OCID1_FLAG, 1级放电过流
	uint8_t CUV:1;   //REGMODE01/R0[5]/0x00/CUV_FLAG, 电池欠压
	uint8_t COV:1;   //REGMODE01/R0[6]/0x00/COV_FLAG, 电池过压
	uint8_t IWF:1;   //REGMODE01/R0[7]/0x00/I2C_WDTO_FLAG, IIC看门狗溢出;
	}bitmap;
	uint8_t cleanflag;
	}R0;
	__attribute__((packed))  struct{//R1
	uint8_t CST:4;  //REGMODE03/R1[3:0]/0x00/CHIP_ST,
	uint8_t CC2F:1; //REGMODE00/R1[4]/0x00/CC2_FLAG,
	uint8_t CC1F:1; //REGMODE00/R1[5]/0x00/CC1_FLAG,
	uint8_t VADF:1; //REGMODE00/R1[6]/0x00/VADC_FLAG,
	uint8_t PD:1;   //REGMODE00/R1[7]/0x00/PACK_DET,
	}R1;
	__attribute__((packed))  struct{
	uint8_t CC1_H;  //REGMODE00/R2[7:0]/0x00/CC1
	uint8_t CC1_L;  //REGMODE00/R3[7:0]/0x00/CC1
	}R2_3;
	__attribute__((packed))  struct{
	uint8_t CC2_H;  //REGMODE00/R4[7:0]/0x00/CC2
	uint8_t CC2_M;  //REGMODE00/R5[7:0]/0x00/CC2
	uint8_t CHGF:1; //REGMODE00/R6[0]/0x00/CHG_FLAG, CHG
	uint8_t DSGF:1; //REGMODE00/R6[1]/0x00/DSG_FLAG, DSG
	uint8_t PCHGF:1;//REGMODE00/R6[2]/0x00/PCHG_FLAG, PCHG
	uint8_t PDSGF:1;//REGMODE00/R6[3]/0x00/PDSG_FLAG, PDSG
	uint8_t CC2_L:4;//REGMODE00/R6[7:4]/0x00/CC2
	}R4_6;
	__attribute__((packed))  struct{//VBAT[15:0], LSB=12.8mV
	uint8_t VBAT_H; //REGMODE00/R7[7:0]/0x00/VBAT
	uint8_t VBAT_L; //REGMODE00/R8[7:0]/0x00/VBAT
	}R7_8;
	__attribute__((packed)) struct{//VPACK LSB=12.8mV
	uint8_t VPK_H; //REGMODE00/R9[7:0]/0x00/VPK
	uint8_t VPK_L; //REGMODE00/R10[7:0]/0x00/VPK
	}R9_10;
	__attribute__((packed)) struct{//VLOAD?[15:0], LSB=12.8mV
	uint8_t VLD_H; //REGMODE00/R11[7:0]/0x00/
	uint8_t VLD_L; //REGMODE00/R12[7:0]/0x00/
	}R11_12;
	__attribute__((packed)) struct{
	uint8_t VCT_H; //REGMODE00/R13[7:0]/0x00/VCT
	uint8_t VCT_L; //REGMODE00/R14[7:0]/0x00/VCT
	}R13_14;
	__attribute__((packed)) struct{
	uint8_t V1P8_H; //REGMODE00/R15[7:0]/0x00/
	uint8_t V1P8_L; //REGMODE00/R16[7:0]/0x00/
	}R15_16;
	__attribute__((packed)) struct
	{
	__attribute__((packed)) struct{
	uint8_t VGP_H; //REGMODE00/R17[7:0]/0x00/
	uint8_t VGP_L; //REGMODE00/R18[7:0]/0x00/
	}VGP[6]; //R17_28
	}R17_28;
	__attribute__((packed)) struct
	{
	__attribute__((packed)) struct{
	uint8_t VCELL_H; //REGMODE00/R29[7:0]/0x00/
	uint8_t VCELL_L; //REGMODE00/R30[7:0]/0x00/
	}VCELLS[24];//R29_R76 register window; each VCELL uses two consecutive registers.
	}R29_76;
	__attribute__((packed)) struct{
	uint8_t VVOS_H; //REGMODE00/R77[7:0]/0x00/VADC
	uint8_t VVOS_L; //REGMODE00/R78[7:0]/0x00/VADC
	}R77_78;
	__attribute__((packed)) struct{
	uint8_t CVOS_H; //REGMODE00/R79[7:0]/0x00/CADC
	uint8_t CVOS_L; //REGMODE00/R80[7:0]/0x00/CADC
	}R79_80;
	__attribute__((packed)) struct{
	uint8_t CHGC:2;  //REGMODE03/R81[1:0]/0x00/CHG_CTRL[1:0],
	uint8_t DSGC:2;  //REGMODE03/R81[3:2]/0x00/DSG_CTRL[1:0],
	uint8_t DSGM:1;  //REGMODE03/R81[4]/0x00/DSG_MODE, DSG
	uint8_t PCHGC:1; //REGMODE03/R81[5]/0x00/PCHG_CTRL, PCHG
	uint8_t PDSGC:1; //REGMODE03/R81[6]/0x00/PDSG_CTRL, PDSG
	uint8_t LDPU:1;  //REGMODE03/R81[7]/0x00/LOAD_PULLUP, LOAD
	}R81;
	__attribute__((packed)) struct{
	uint8_t DPC:5;  //REGMODE03/R82[4:0]/0x10/DSG_PD_CTRL[4:0],
	uint8_t R82_RVD:2;
	uint8_t PDWM:1; //REGMODE03/R82[7]/0x01/PDSG_I2C_WDTO_MASK,
	}R82;
	__attribute__((packed)) struct{
	uint8_t DBDM:1;  //REGMODE03/R83[0]/0x00/DSG_BODY_DIODE_MASK,
	uint8_t DPDM:1;  //REGMODE03/R83[1]/0x00/DSG_PDSG_MASK, PDSG
	uint8_t DDM:1;   //REGMODE03/R83[2]/0x00/DSG_DOFFN_MASK, DON(DSG_OFF_N)
	uint8_t DWM:1;   //REGMODE03/R83[3]/0x00/DSG_I2C_WDTO_MASK,
	uint8_t PCDM:1;  //REGMODE03/R83[4]/0x01/PCHG_DON_MASK, DON(DSG_OFF_N)
	uint8_t PCCM:1;  //REGMODE03/R83[5]/0x00/PCHG_CON_MASK, CON(CHG_OFF_N)
	uint8_t PCWM:1;  //REGMODE03/R83[6]/0x00/PCHG_I2C_WDTO_MASK,
	uint8_t PDDM:1;  //REGMODE03/R83[7]/0x00/PDSG_DON_MASK, DON(DSG_OFF_N)
	}R83;
	__attribute__((packed)) struct{
	uint8_t CBDM:1;  //REGMODE03/R84[0]/0x00/CHG_BODY_DIODE_MASK,
	uint8_t CPCM:1;  //REGMODE03/R84[1]/0x00/CHG_PCHG_MASK,
	uint8_t CCM:1;   //REGMODE03/R84[2]/0x00/CHG_CON_MASK,
	uint8_t CDM:1;   //REGMODE03/R84[3]/0x01/CHG_DON_MASK,
	uint8_t CSM:1;   //REGMODE03/R84[4]/0x01/CHG_SCD_MASK,
	uint8_t CO2M:1;  //REGMODE03/R84[5]/0x01/CHG_OCD2_MASK,
	uint8_t CO1M:1;  //REGMODE03/R84[6]/0x01/CHG_OCD1_MASK,
	uint8_t CWM:1;   //REGMODE03/R84[7]/0x00/CHG_I2C_WDTO_MASK,
	}R84;


	__attribute__((packed)) struct{
		uint8_t CAMZ:1;  //REGMODE02/R85[0]/0x00/CADC_MZ, CADC
		uint8_t R85_RVD1:1;
		uint8_t CAES:1;  //REGMODE03/R85[2]/0x01/CADC_EN_SLEEP,
		uint8_t CAEW:1;  //REGMODE03/R85[3]/0x01/CADC_EN_WORK,
		uint8_t R85_RVD4_6:3;
		uint8_t HSFM:1;  //REGMODE03/R85[7]/0x00/HSFET_MASK,
	}R85;
	__attribute__((packed)) struct{
		uint8_t C1OS:2;  //REGMODE03/R86[1:0]/0x03/CC1_OSR_SLEEP[1:0],  128;01, 256;10, 512;11, 1024
		uint8_t C1OW:2;  //REGMODE03/R86[3:2]/0x03/CC1_OSR_WORK[1:0],  128;01, 256;10, 512;11, 1024
		uint8_t R86_RVD:4;
	}R86;
	uint8_t R87;
	uint8_t R88;
	__attribute__((packed)) struct{
		uint8_t OCD1T; //REGMODE03/R89[7:0]/0x00/OCD1_TH[7:0],  V(OCD1_TH)=OCD1T*0.25mV
	}R89;
	__attribute__((packed)) struct{
		uint8_t OCC1T; //REGMODE03/R90[7:0]/0xFF/OCC1_TH[7:0],  V(OCC1_TH)=OCD1T*0.25mV
	}R90;
	__attribute__((packed)) struct{
		uint8_t OCD1D; //REGMODE03/R91[7:0]/0x00/OCD1_DLY[7:0],  T(OCD1_DLY)=OCD1D*8ms
	}R91;
	__attribute__((packed)) struct{
		uint8_t OCC1D; //REGMODE03/R92[7:0]/0x00/OCC1_DLY[7:0], T(OCD1_DLY)=OCD1D*8ms
	}R92;
	uint8_t R93;
	__attribute__((packed)) struct{
		uint8_t OCD2T:6;  //REGMODE03/R94[5:0]/0x00/OCD2_TH[5:0], =(OCD2T+1)*4mV
		uint8_t OCD2E:1;  //REGMODE03/R94[6]/0x01/OCD2_EN,
		uint8_t R94_RVD:1;
	}R94;
	__attribute__((packed)) struct{
		uint8_t OCC2T:6;  //REGMODE03/R95[5:0]/0x00/OCC2_TH[7:0], =(OCC2T+1)*4mV
		uint8_t OCC2E:1;  //REGMODE03/R95[6]/0x01/OCC2_EN,
		uint8_t R95_RVD:1;
	}R95;
	__attribute__((packed)) struct{
		uint8_t OCD2D;//REGMODE03/R96[7:0]/0x00/OCD2_DLY[7:0], =OCD2D*4ms
	}R96;
	__attribute__((packed)) struct{
	uint8_t OCC2D;//REGMODE03/R97[7:0]/0x00/OCC2_DLY[7:0], =OCC2D*4ms
	}R97;
	__attribute__((packed)) struct{
	uint8_t SCDT:6;  //REGMODE03/R98[5:0]/0x00/SCD_VTH[5:0], =SCDT*10mV
	uint8_t SCDE:1;  //REGMODE03/R98[6]/0x01/SCD_EN,
	uint8_t R98_RVD:1;
	}R98;
	__attribute__((packed)) struct{
	uint8_t SCDD;//REGMODE03/R99[7:0]/0x00/SCD_DLY[7:0], =SCDD*7.81us
	}R99;
	uint8_t R100;
	__attribute__((packed)) struct{
	uint8_t CWT;   //REGMODE03/R101[7:0]/0x00/CURRENT_WKUP_TH[7:0],
	}R101;
	__attribute__((packed)) struct{
	uint8_t BDPT;  //REGMODE03/R102[7:0]/0x00/BODY_DIODE_PRT_TH[7:0],
	}R102;
	__attribute__((packed)) struct{
	uint8_t CB[3]; //REGMODE03/R103[7:0],R104[7:0],R105[7:0]/0x00/CELL_BAL[17:1],
	}R103_R105;
	__attribute__((packed)) struct{
	uint8_t CM_H;    //REGMODE03/R106[7:0]/0xAA/
	}R106;
	__attribute__((packed)) struct{
	uint8_t CM_M;    //REGMODE03/R107[7:0]/0x55/
	}R107;
	__attribute__((packed)) struct{
	uint8_t V1P8M:1; //REGMODE03/R108[0]/0x00/V1P8_MASK, V1P8
	uint8_t CTM:1;   //REGMODE03/R108[1]/0x00/CORE_TEMP_MASK,
	uint8_t LDM:1;   //REGMODE03/R108[2]/0x00/LOAD_MASK, LOAD
	uint8_t PKM:1;   //REGMODE03/R108[3]/0x00/PACK_MASK, PACK
	uint8_t CM_L:4;    //REGMODE03/R108[7:4]/0x00/
	}R108;
	__attribute__((packed)) struct{
	uint8_t CVS:1; //REGMODE03/R109[0]/0x00/CELL_VOLTAGE_SIGNED,
	uint8_t CMM:1; //REGMODE03/R109[1]/0x00/ELL_MASK_MEAS,
	uint8_t COW:1; //REGMODE03/R109[2]/0x00/CELL_OPEN_WIRE,
	uint8_t CPVS:3;//REGMODE03/R109[5:3]/0x05/CP_VS[2:0],
	uint8_t CUWM:1;//REGMODE03/R109[6]/0X00/CUV_WATCHDOG_MODE,
	uint8_t R109_RVD:1;
	}R109;
	__attribute__((packed)) struct{
	uint8_t VAO:2;  //REGMODE03/R110[1:0]/0x01/VADC_OSR[1:0],
	uint8_t R110_RVD:2;
	uint8_t VAMP:2; //REGMODE03/R110[5:4]/0x00/VADC_MEAS_PERIOD[1:0],
	uint8_t VASM:1; //REGMODE03/R110[6]/0x01/VADC_SYNC_MEAS_EN,
	uint8_t VAE:1;  //REGMODE03/R110[7]/0x01/VADC_EN,
	}R110;
	uint8_t R111;
	__attribute__((packed)) struct{
	uint8_t COVT_H;  //REGMODE03/R112[7:0]/0x00/
	}R112;
	__attribute__((packed)) struct{
	uint8_t COVD:4;  //REGMODE03/R113[3:0]/0x00/CELL_OV_DLY[3:0], 200ms;0001, 300ms;0010, 400ms;0011, 500ms;0100, 600ms;0101, 700ms;0110, 800ms;0111, 900ms;1000, 1s;1001, 2s;1010, 3s;1011, 4s;1100, 5s;1101, 6s;1110, 7s;1111, 8s
	uint8_t COVT_L:4;//REGMODE03/R113[7:4/0x00/
	}R113;
	__attribute__((packed)) struct{
	uint8_t CUVT_H;  //REGMODE03/R114[7:0]/0x00/
	}R114;
	__attribute__((packed)) struct{
	uint8_t CUVD:4;  //REGMODE03/R115[3:0]/0x00/CELL_UV_DLY[3:0], 200ms;0001, 300ms;0010, 400ms;0011, 500ms;0100, 600ms;0101, 700ms;0110, 800ms;0111, 900ms;1000, 1s;1001, 2s;1010, 3s;1011, 4s;1100, 5s;1101, 6s;1110, 7s;1111, 8s
	uint8_t CUVT_L:4;//REGMODE03/R115[7:4]/0x00/
	}R115;
	__attribute__((packed)) struct{
	uint8_t GP3M:3;  //REGMODE03/R116[2:0]/0x00/GP3_MODE[2:0], GP3
	uint8_t GP2M:3;  //REGMODE03/R116[5:3]/0x00/GP2_MODE[2:0], GP2
	uint8_t GP1M:2;  //REGMODE03/R116[7:6]/0x00/GP1_MODE[1:0], GP1
	}R116;

	__attribute__((packed)) struct{
	uint8_t GP6M:3; //REGMODE03/R117[2:0]/0x00/GP6_MODE[2:0], GP6
	uint8_t GP5M:3; //REGMODE03/R117[5:3]/0x00/GP5_MODE[2:0], GP5
	uint8_t GP4M:2; //REGMODE03/R117[7:6]/0x00/GP4_MODE[1:0], GP4
	}R117;
	__attribute__((packed)) struct{
	uint8_t COTT:7;  //REGMODE03/R118[6:0]/0x00/CORE_OVER_TEMP_TH[6:0],
	uint8_t COTF:1;  //REGMODE00/R118[7]/0x00/CORE_OVER_TEMP_FLAG,
	}R118;
	__attribute__((packed)) struct{
	uint8_t IWT:3;   //REGMODE03/R119[2:0]/0x00/I2C_WATCHDOG_TIMER[2:0],
	uint8_t IWTS:1;  //REGMODE00/R119[3]/0x00/I2C_WATCHDOG_TIMEOUT_STATE,
	uint8_t R119_RVD4:1;
	uint8_t V3P3M:1; //REGMODE03/R119[5]/0x00/V3P3_WDTO_MODE,
	uint8_t V3P3EW:1; //REGMODE03/R119[6]/0x01/V3P3_EN_WORKING,
	uint8_t V3P3ES:1; //REGMODE03/R119[7]/0x00/V3P3_EN_SLEEPING,
	}R119;
	__attribute__((packed)) struct{
	uint8_t TWSE:4; //REGMODE03/R120[3:0]/0x00/TIMED_WAKEUP_TIMER[3:0],
	uint8_t R120_RVD:3;
	uint8_t TIWK:1; //REGMODE00/R120[7]/0x00/TIMED_WAKEUP_STATE,
	}R120;
	__attribute__((packed)) struct{
	uint8_t ISCM:1;  //REGMODE03/R121[0]/0x00/INT_SCD_MASK,
	uint8_t IOC2M:1; //REGMODE03/R121[1]/0x00/INT_OC2_MASK,
	uint8_t IOC1M:1; //REGMODE03/R121[2]/0x00/INT_OC1_MASK,
	uint8_t ICUM:1;  //REGMODE03/R121[3]/0x00/CUV_MASK,
	uint8_t ICOM:1;  //REGMODE03/R121[4]/0x00/INT_COV_MASK,
	uint8_t ICCM:1;  //REGMODE03/R121[5]/0x00/INT_CADC_CC2_MASK, CADC CC2?
	uint8_t IVOM:1;  //REGMODE03/R121[6]/0x00/INT_VADC_OVER_MASK, VADC
	}R121;
	uint8_t R122;
	uint8_t R123;
	uint8_t R124;
	uint8_t R125;
	__attribute__((packed)) struct{
	uint8_t F1RT; //REGMODE13/R126[7:0]/0x80/FUSE1_RES_TRIM[7:0],
	}R126;
	uint8_t R127;
	uint8_t R128;
	uint8_t R129;
	uint8_t R130;
	uint8_t R131;
	uint8_t R132;
	uint8_t R133;
	uint8_t R134;
	uint8_t R135;
	uint8_t R136;
	uint8_t R137;
	uint8_t R138;
	uint8_t R139;
	uint8_t R140;
	uint8_t R141;
	uint8_t R142;
	__attribute__((packed)) struct{
		uint8_t CV;//REGMODE03/R143[7:0/0x00/CHIP_VERSION[7:0],
	}R143;
	uint8_t R144;

}TAFERegs;
//---------------------------------------------------------------------------
#pragma pack (pop)

static const unsigned char DVC11XX_PresetConfigRegData_R81To121[] =
/*20串 感应电阻0.25mΩ */
{
	0x0A,  			//0x51  0000 1010   
	0x9E,  			//0x52  1001 1110  下拉强度
	0x50,  			//0x53  0101 0000  
	0x78,  			//0x54  0111 1000
 	0xAC,  			//0x55  1010 1100   屏蔽高边FET驱动输出  
 	0xFF,0x3F,0x05, //0x56~0x58
	0xB4,		    //0x59   B4(180) * 0.25 /0.25 = 180A   一级OCD
	0x3C,           //0x5A   3C(60) * 0.25 /0.25 = 60A  一级OCC	
	0x25,           //0x5B   25 (37 + 1) * 8 = 304ms    一级Time
	0x25,           //0x5C   25 (37 + 1) * 8 = 304ms    一级Time
	0xB0,           //0x5D
	0x4B,           //0x5E   1101  ENABLE 0x0B (11+1) * 4 /0.25 = 192A  二级OCD
 	0x44,           //0x5F   0100  EMABLE 0x04 (4+1) * 4 /0.25 = 80A  二级OCC
	0x18,           //0x60   18(24+1) * 4 = 100ms   二级Time
	0x18,           //0x61   18(24+1) * 4 = 100ms   二级Time
	0x4A,		    //0x62   SCDE = 1  SCDT[0~5] 001010 = 10 * 10 / 0.25 = 400A 短路参数 
	0x33,0x88,      //0x63~0x64   33(51) * 7.81 = 398.31 us
	0x14,           //0x65   20 * 10 /0.2 / 1000 = 1A  休眠下的唤醒电流
	0x06,           //0x66   6 * 40 /0.2/1000 = 1.2A  续流保护
	0x87,0xFF,0xFF, //0x67~0x69 均衡，20S  顶节是[24]
	0x78,0x00,0x00, //0x6A~0x6C 电芯屏蔽  20S
	0x48,			//0x6D  
	0xDF,0x28,      //0x6E~0x6F 
	0xF0,0xA9,      //0x70~0x71   3850*1+500 4350mV   2s
	0x7D,0x09,      //0x72~0x73   2000*1 = 2000mV     2s
	0xF1,           //0x74        
	0xFF,           //0x75     
	0x3F,           //0x76
	0xC0,		    //0x77
	0x00,           //0x78
	0x60			//0x79
};
//------------------------------------------------------------------------------

bool hsi_afe_dvc1124_ReadDataWithCRC(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t *dataPtr,uint16_t dataLen);
bool hsi_afe_dvc1124_WriteDataWithCRC(m_soft_i2c_t *i2c, uint8_t regAddr,void *dataPtr,uint16_t dataLen);
bool hsi_afe_dvc1124_ReadRegs(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t regLen);
bool hsi_afe_dvc1124_WriteRegs(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t regLen);
bool hsi_afe_dvc1124_CleanError(m_soft_i2c_t *i2c, uint8_t err_mask);
bool hsi_afe_dvc1124_Reset(m_soft_i2c_t *i2c);
bool hsi_afe_dvc1124_Init(m_soft_i2c_t *i2c);
bool hsi_afe_dvc1124_Sleep(m_soft_i2c_t *i2c);
bool hsi_afe_dvc1124_ShutDown(m_soft_i2c_t *i2c);

bool hsi_afe_dvc1124_CalcCurrent(m_soft_i2c_t *i2c, int32_t *cur, int32_t *reg);
hsi_afe_dvc1124_ntc_status_t hsi_afe_dvc1124_CalcTemp(m_soft_i2c_t *i2c, int16_t *temp);
void hsi_afe_dvc1124_CalcVPACK(m_soft_i2c_t *i2c, uint32_t *vpack);
void hsi_afe_dvc1124_CalcVBAT(m_soft_i2c_t *i2c, uint32_t *vbat);
int8_t hsi_afe_dvc1124_CalcVolCell(m_soft_i2c_t *i2c, uint16_t *vol, uint8_t num);
int8_t hsi_afe_dvc1124_CalcVolCell_Optional(m_soft_i2c_t *i2c, uint16_t *vol, uint8_t seq, uint8_t num);
int8_t hsi_afe_dvc1124_ChMosCtl(m_soft_i2c_t *i2c, uint16_t Status);
int8_t hsi_afe_dvc1124_DchMosCtl(m_soft_i2c_t *i2c, uint16_t Status);
int8_t hsi_afe_dvc1124_MosCtl(m_soft_i2c_t *i2c, uint8_t CMOS, uint8_t DMOS);
int8_t hsi_afe_dvc1124_Balance(m_soft_i2c_t *i2c, uint32_t u32CellMask);
int8_t hsi_afe_dvc1124_GetStatus(m_soft_i2c_t *i2c);
int8_t hsi_afe_dvc1124_CheckRST(m_soft_i2c_t *i2c);

//------------------------------------------------------------------------------
extern volatile TAFERegs gt_hsi_afe_dvc1124_regs;
extern m_soft_i2c_t gt_hsi_afe_dvc1124_i2c;
//------------------------------------------------------------------------------
#endif
