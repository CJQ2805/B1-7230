/*
 * 文件作用：
 *   DVC1124芯片寄存器驱动层，负责具体寄存器读写、CRC/PEC处理和原始数据换算。
 *
 * 主要用途：
 *   1. 初始化DVC1124寄存器配置。
 *   2. 读取单体电压、电流、VBAT、温度和状态寄存器。
 *   3. 执行DVC1124专有的MOS控制、均衡、清故障、休眠和关断命令。
 *
 * 适用范围：
 *   只适用于DVC1124芯片。更换AFE时优先替换本文件，并保持 hsi_afe 对上接口稳定。
 *
 * 调用关系：
 *   hsi_afe.c 调用本文件；本文件通过 Middle/d_bus 访问AFE IIC总线。
 *
 * 注意事项：
 *   本文件可以包含DVC1124寄存器细节，但这些细节不应继续向App业务模块扩散。
 */
#include "hsi_afe_dvc1124.h"
#include "Middle/m_delay.h"
#include "hsi_ntc.h"
#include "RTT/SEGGER_RTT.h"

#define IIC_SLAVE_ADDRESS 0x40
#define NoUsed 0
//------------------------------------------------------------------------------
/* gt_hsi_afe_dvc1124_regs用于镜像IIC读取后的DVC1124寄存器窗口。 */
volatile TAFERegs gt_hsi_afe_dvc1124_regs;//寄存器配置存储位置
/* 共用传输缓存：每个数据字节后面跟1个CRC字节。 */
static uint8_t I2CTransferBuffer[300];
#define HSI_AFE_DVC1124_I2C_MAX_READ_DATA  ((uint16_t)(sizeof(I2CTransferBuffer) / 2U))
#define HSI_AFE_DVC1124_I2C_MAX_WRITE_DATA ((uint16_t)((sizeof(I2CTransferBuffer) - 2U) / 2U))
/* 单体校准缓存，芯片初始化后复位。 */
static float hsi_afe_dvc1124_cell_cal_cache[AFE_MAX_CELL_CNT];

/* DVC1124的VCELL寄存器连续排列：VCELL1从0x1D开始，每节占两个寄存器。 */
static uint8_t hsi_afe_dvc1124_cell_reg_addr(uint8_t u8CellIndex)
{
	return (uint8_t)(0x1D + ((uint16_t)u8CellIndex * 2U));
}

static uint32_t hsi_afe_dvc1124_cell_mask(uint8_t u8CellCount)
{
	if(u8CellCount >= AFE_MAX_CELL_CNT)
	{
		return (1UL << AFE_MAX_CELL_CNT) - 1UL;
	}

	if(u8CellCount == 0U)
	{
		return 0U;
	}

	return (1UL << u8CellCount) - 1UL;
}

static uint16_t hsi_afe_dvc1124_cell_raw(uint8_t u8CellIndex)
{
	uint8_t u8RegisterAddress = hsi_afe_dvc1124_cell_reg_addr(u8CellIndex);
	volatile uint8_t *pu8RegisterBytes = (volatile uint8_t *)&gt_hsi_afe_dvc1124_regs;

	return ((uint16_t)pu8RegisterBytes[u8RegisterAddress] << 8) | pu8RegisterBytes[u8RegisterAddress + 1U];
}

static uint8_t hsi_afe_dvc1124_cell_read_len(uint8_t u8StartCellIndex, uint8_t u8CellCount)
{
	uint8_t u8FirstRegisterAddress = hsi_afe_dvc1124_cell_reg_addr(u8StartCellIndex);
	uint8_t u8LastRegisterAddress = hsi_afe_dvc1124_cell_reg_addr(u8StartCellIndex + u8CellCount - 1U) + 1U;

	return (uint8_t)(u8LastRegisterAddress - u8FirstRegisterAddress + 1U);
}

static void hsi_afe_dvc1124_reset_cell_cal_cache(void)
{
	for(uint8_t u8CellIndex = 0U; u8CellIndex < AFE_MAX_CELL_CNT; u8CellIndex++)
	{
		hsi_afe_dvc1124_cell_cal_cache[u8CellIndex] = 0.0f;
	}
}

static float hsi_afe_dvc1124_raw_to_cell_volt(uint16_t u16RawAdcValue)
{
	if(gt_hsi_afe_dvc1124_regs.R109.CVS)
	{
		return _lsbVCELL_signed * (int16_t)u16RawAdcValue;
	}

	return _lsbVCELL * u16RawAdcValue;
}

//----------------------CRC8查值表-----------------------------------------------------
const uint8_t CRC8_LOOKUP_TABLE[256] =
{
	0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
	0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
	0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
	0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
	0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
	0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
	0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
	0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
	0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
	0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
	0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
	0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
	0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
	0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
	0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
	0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3
};

/**************************************************************************************
date        : 2024-11-11
Author      : xiaoai
parameter   : dataPtr - 数据地址
: dataLen - 长度
retval      : CRC8
function    : 查表法计算CRC g(x)=x8+x2+x+1
**************************************************************************************/
uint8_t Calc_Crc8withTable(uint8_t *dataPtr,uint16_t dataLen)
{
	uint8_t u8Crc8 = 0U;
	uint8_t *pu8Data = dataPtr;
	while(dataLen--)
	{
		u8Crc8 = CRC8_LOOKUP_TABLE[u8Crc8 ^ *pu8Data++];
	}
	return u8Crc8;
}

/**************************************************************************************
date        : 2024-11-4
Author      : xiaoai
parameter   : chkSum - 上一次的校验值
: crcData - 待校验数据
retval      : CRC8
function    : 单次计算CRC8值 g(x)=x8+x2+x+1
**************************************************************************************/
#define POLYNOME            0x07

/* 逐字节CRC8计算，保留用于兼容和调试。 */
uint8_t Calc_Crc8(uint8_t chkSum, uint8_t crcData)
{
	uint8_t u8BitCount = 8U;        // Counter for 8 shifts

	chkSum ^= crcData;              // Initial XOR
	do
	{
		if (!(chkSum & 0x80))        // 检查最高位
		{
			chkSum = chkSum << 1;   // If MSB = 0, shift left
		}
		else
		{
			chkSum = (chkSum << 1) ^ POLYNOME;  // If MSB = 1, shift and XOR
		}
	}
	while (--u8BitCount);                                 // Continue for 8 bits
	return chkSum;                             // 返回最终CRC值
}

/* 读取DVC1124寄存器数据，每个数据字节后带1个CRC字节。 */
bool hsi_afe_dvc1124_ReadDataWithCRC(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t *dataPtr,uint16_t dataLen)
{
	uint8_t au8DummyWrite[4];
	uint8_t *pu8SourceData;

	if((i2c == 0) || (dataPtr == 0) || (dataLen == 0U) ||
	   (dataLen > HSI_AFE_DVC1124_I2C_MAX_READ_DATA))
	{
		return false;
	}

	au8DummyWrite[0] = IIC_SLAVE_ADDRESS; //写入设备地址
	au8DummyWrite[1] = regAddr;  //寄存器地址
	//乘以2是因为一个字节数据绑定一个字节CRC，所以左移一位代表乘以2
	if(m_soft_i2c_read(i2c, IIC_SLAVE_ADDRESS, regAddr, 8, I2CTransferBuffer, dataLen<<1))
	{  //先写后读
		au8DummyWrite[2] = (IIC_SLAVE_ADDRESS)|0x1;//SA+R
		au8DummyWrite[3] = I2CTransferBuffer[0];//DATA0
		if(I2CTransferBuffer[1] == Calc_Crc8withTable(au8DummyWrite,4))//CRC0校验
		{
			int i32RemainingByteCount;
			if(dataLen > 1U)
			{
				for(i32RemainingByteCount = (int)dataLen - 1, pu8SourceData = I2CTransferBuffer + 2;
					i32RemainingByteCount > 0;
					i32RemainingByteCount--, pu8SourceData += 2)
				{
					if(pu8SourceData[1] != Calc_Crc8withTable(pu8SourceData,1))
					{
						return false;//读取失败（CRC错误）
					}
				}
				for(i32RemainingByteCount = (int)dataLen, pu8SourceData = I2CTransferBuffer;
					i32RemainingByteCount > 0;
					i32RemainingByteCount--, pu8SourceData += 2)
				{
					*(uint8_t *)dataPtr = *pu8SourceData;
					dataPtr = (uint8_t *)dataPtr+1;
				}
				return true; //读取成功
			}
			else if(dataLen == 1)
			{
				*dataPtr = I2CTransferBuffer[0];
				return true; //读取成功
			}
		}
	}
	return false;
}

/**************************************************************************************
date        : 2024-11-1
Author      : xiaoai
parameter   : regAddr - AFE寄存器地址
: dataPtr - 数据地址
: dataLen - 长度
retval      : 成功/失败
function    : IIC
**************************************************************************************/
/* 写DVC1124寄存器数据，并附加芯片要求的CRC字节。 */
bool hsi_afe_dvc1124_WriteDataWithCRC(m_soft_i2c_t *i2c, uint8_t regAddr,void *dataPtr,uint16_t dataLen)
{
	uint8_t *pu8Destination;
	uint8_t *pu8Source;

	if((i2c == 0) || (dataPtr == 0) || (dataLen == 0U) ||
	   (dataLen > HSI_AFE_DVC1124_I2C_MAX_WRITE_DATA))
	{
		return false;
	}

	pu8Destination = I2CTransferBuffer;
	pu8Source = (uint8_t *)dataPtr;
	*pu8Destination++ = IIC_SLAVE_ADDRESS; //(从设备设备地址<<)|写操作
	*pu8Destination++ = regAddr;  //寄存器地址
	*pu8Destination++ = *pu8Source++;
	*pu8Destination++ = Calc_Crc8withTable(I2CTransferBuffer,3);
	while(--dataLen>0)
	{
		*pu8Destination++ = *pu8Source;
		*pu8Destination++ = Calc_Crc8withTable(pu8Source++,1);
	}

	return m_soft_i2c_write(i2c, IIC_SLAVE_ADDRESS, regAddr, 8, I2CTransferBuffer+2, (uint16_t)(pu8Destination - I2CTransferBuffer - 2U));

}

/**************************************************************************************
date        : 2024-11-1
Author      : xiaoai
parameter   : regAddr - AFE寄存器起始地址
: regLen - 数据长度
retval      : 成功/失败
function    : AFE数据读取
**************************************************************************************/
/* 将寄存器窗口读入gt_hsi_afe_dvc1124_regs；失败立即重试，不在周期任务里插入5ms阻塞延时。 */
bool hsi_afe_dvc1124_ReadRegs(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t regLen)
{
	int i32RetryCount;
	for(i32RetryCount = 3; i32RetryCount > 0; i32RetryCount--)
	{
		if(hsi_afe_dvc1124_ReadDataWithCRC(i2c, regAddr,(uint8_t *)&gt_hsi_afe_dvc1124_regs+regAddr, regLen))
		{
			return true;
		}
	}
	return false;
}

/**************************************************************************************
date        : 2024-11-1
Author      : xiaoai
parameter   : regAddr - AFE寄存器起始地址
: regLen - 数据长度
retval      : 成功/失败
function    : AFE数据写入
**************************************************************************************/
/* 将gt_hsi_afe_dvc1124_regs中的寄存器窗口写回芯片；失败立即重试，避免影响5ms/10ms任务时机。 */
bool hsi_afe_dvc1124_WriteRegs(m_soft_i2c_t *i2c, uint8_t regAddr,uint8_t regLen)
{
	int i32RetryCount;
	for(i32RetryCount = 3; i32RetryCount > 0; i32RetryCount--)
	{
		if(hsi_afe_dvc1124_WriteDataWithCRC(i2c, regAddr,(uint8_t *)&gt_hsi_afe_dvc1124_regs+regAddr, regLen))
		{
			return true;
		}
	}
	return false;
}

/**************************************************************************************
date        : 2024-11-1
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE强制关机
****/
bool hsi_afe_dvc1124_ShutDown(m_soft_i2c_t *i2c)
{
	gt_hsi_afe_dvc1124_regs.R1.CST = 0x0f;
	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(1),1);
}
/***
date        : 2024-11-1
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE强制休眠
**************************************************************************************/
bool hsi_afe_dvc1124_Sleep(m_soft_i2c_t *i2c)
{
	if(i2c == 0)
	{
		return false;
	}

	gt_hsi_afe_dvc1124_regs.R1.CST = 0xE;//配置睡眠
	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(1),1);
}

/**************************************************************************************
date        : 2024-11-5
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE清除故障标志
**************************************************************************************/
bool hsi_afe_dvc1124_CleanError(m_soft_i2c_t *i2c, uint8_t err_mask)
{
	if(i2c == 0)
	{
		return false;
	}

	gt_hsi_afe_dvc1124_regs.R0.cleanflag = err_mask;
	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(0), 1);
}

/**************************************************************************************
date        : 2024-11-5
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE复位
**************************************************************************************/
bool hsi_afe_dvc1124_Reset(m_soft_i2c_t *i2c)
{
	gt_hsi_afe_dvc1124_regs.R1.CST = 0xd;
	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(1), 1);
}

/**************************************************************************************
date        : 2024-11-5
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE初始化寄存器
**************************************************************************************/
/* 装载R81..R121预设配置，并一次写入整段配置窗口。 */
static bool hsi_afe_dvc1124_InitRegs(m_soft_i2c_t *i2c)
{
	bool bWriteResult;
	bool bReadbackResult;

	memcpy((char *)&gt_hsi_afe_dvc1124_regs+81,DVC11XX_PresetConfigRegData_R81To121,sizeof(DVC11XX_PresetConfigRegData_R81To121));
	bWriteResult = hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(81),sizeof(DVC11XX_PresetConfigRegData_R81To121));
	if (bWriteResult == false)
	{
		SEGGER_RTT_WriteString(0, "AFE OCC2 cfg write failed\r\n");
		return false;
	}

	/* 只在初始化时读回一次，确认OCC2门限和延时确实写入AFE。 */
	bReadbackResult = hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0x5F), 3U);
	if (bReadbackResult)
	{
		SEGGER_RTT_printf(0, "AFE OCC2 cfg R95=%02X R97=%02X\r\n",
					  (unsigned int)AFE_REG_VALUE(R95),
					  (unsigned int)AFE_REG_VALUE(R97));
	}
	else
	{
		SEGGER_RTT_WriteString(0, "AFE OCC2 cfg readback failed\r\n");
	}

	return true;
}

/**************************************************************************************
date        : 2022-04-11
Author      : aijinqiu
parameter   : i2c - IIC外设
retval      : 初始化结果
function    : dvc1124初始化，包含：
:   - IIC初始化
:   - 配置寄存器参数
**************************************************************************************/
/* 先复位，等待AFE内部启动，再写入项目默认寄存器配置。 */
bool hsi_afe_dvc1124_Init(m_soft_i2c_t *i2c)
{
	bool bInitResult = false;

	m_soft_i2c_init(i2c);//IIC初始化+唤醒
	if((bInitResult = hsi_afe_dvc1124_Reset(i2c)) == false)
	{
	   return bInitResult; //复位失败就退出重试
	}
	m_delay_ms(300);//300ms延时等待AFE初始化
	//实际延时一定要大于280ms-300ms

	if((bInitResult = hsi_afe_dvc1124_InitRegs(i2c)) == false)
	{
	   return bInitResult; //初始化失败就退出重试
	}

	hsi_afe_dvc1124_CleanError(i2c, 0x00);//写0清除

	return bInitResult;
}

/**************************************************************************************
date        : 2024-11-6
Author      : xiaoai
parameter   : value采样值；cellno电芯序号
retval      : 校准后值
function    : DVC1124专用电压二次校准策略
**************************************************************************************/
/* DVC1124单体电压修正依赖低位单体累计共模电压。 */
static float CellVolSecondaryCalibrate(uint16_t u16RawAdcValue, uint8_t u8CellIndex)
{
	const float fCalibrationP1 = 0.35f / 1000000.0f;
	const float fCalibrationP2 = -0.12f / 1000000.0f;
	float fRawCellVoltage = hsi_afe_dvc1124_raw_to_cell_volt(u16RawAdcValue);
	float fCommonModeVoltage = 0.0f;
	float fCalibratedCellVoltage = fRawCellVoltage;

	for(uint8_t u8LowerCellIndex = 0U; u8LowerCellIndex < u8CellIndex && u8LowerCellIndex < AFE_MAX_CELL_CNT; u8LowerCellIndex++)
	{
		fCommonModeVoltage += hsi_afe_dvc1124_cell_cal_cache[u8LowerCellIndex];
	}

	if(u8CellIndex > 0U)
	{
		fCalibratedCellVoltage = fRawCellVoltage / (1.0f + fCalibrationP1 * fCommonModeVoltage + fCalibrationP2 * fCommonModeVoltage * fCommonModeVoltage);
	}

	if(u8CellIndex < AFE_MAX_CELL_CNT)
	{
		hsi_afe_dvc1124_cell_cal_cache[u8CellIndex] = fCalibratedCellVoltage;
	}

	return fCalibratedCellVoltage;

}

/**************************************************************************************
date        : 2024-11-6
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE电压全读
**************************************************************************************/
int8_t hsi_afe_dvc1124_CalcVolCell(m_soft_i2c_t *i2c, uint16_t *vol, uint8_t num)
{
	return hsi_afe_dvc1124_CalcVolCell_Optional(i2c, vol, 0U, num);
}

/**************************************************************************************
date        : 2025-2-14
Author      : xiaoai
parameter   : NA
retval      : NA
function    : AFE读取电压
**************************************************************************************/
/* 按DVC1124连续单体寄存器映射读取一段单体电压。 */
int8_t hsi_afe_dvc1124_CalcVolCell_Optional(m_soft_i2c_t *i2c, uint16_t *vol, uint8_t seq, uint8_t num)
{
	bool bReadResult;
	float fCalibratedCellVoltage;
	uint8_t u8RegisterAddress;
	uint8_t u8RegisterLength;

	if((i2c == 0) || (vol == 0) || (num == 0U) || (seq >= AFE_MAX_CELL_CNT))
	{
		return -1;
	}

	if((seq + num) > AFE_MAX_CELL_CNT)
	{
		num = AFE_MAX_CELL_CNT - seq;
	}

	if(seq == 0U)
	{
		hsi_afe_dvc1124_reset_cell_cal_cache();
	}

	u8RegisterAddress = hsi_afe_dvc1124_cell_reg_addr(seq);
	u8RegisterLength = hsi_afe_dvc1124_cell_read_len(seq, num);
	bReadResult = hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(u8RegisterAddress), u8RegisterLength);

	if(!bReadResult) return -1;

	for(uint8_t u8CellIndex = seq; u8CellIndex < (seq + num); u8CellIndex++)
	{
		fCalibratedCellVoltage = CellVolSecondaryCalibrate(hsi_afe_dvc1124_cell_raw(u8CellIndex), u8CellIndex);
		vol[u8CellIndex] = (uint16_t)(fCalibratedCellVoltage * 1000.0f + 0.5f);
	}

	return 0;

}

/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : NA
retval      : NA
function    : 获取AFE片内温度
**************************************************************************************/
int8_t hsi_afe_dvc1124_ICTemp(m_soft_i2c_t *i2c, int16_t *temp)
{
	bool bReadResult;
	uint16_t u16VctRaw = 0U;

	bReadResult = hsi_afe_dvc1124_ReadDataWithCRC(i2c, AFE_ADDR_R(13), (uint8_t *)&gt_hsi_afe_dvc1124_regs+AFE_ADDR_R(13), 2);//VCT寄存器获取

	if(!bReadResult) return -1;

	u16VctRaw = (uint16_t)((gt_hsi_afe_dvc1124_regs.R13_14.VCT_H << 8) | gt_hsi_afe_dvc1124_regs.R13_14.VCT_L);

	*temp = (int16_t)(u16VctRaw * 0.24467f - 271.3f);//摄氏度

	return 0;

}

/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : temp ：温度值
retval      : NA
function    : 获取GP口采样温度
**************************************************************************************/

/* 根据GP3电压、1.8V参考和修调上拉电阻计算NTC温度，并返回异常来源。 */
hsi_afe_dvc1124_ntc_status_t hsi_afe_dvc1124_CalcTemp(m_soft_i2c_t *i2c, int16_t *temp)
{
	float fV1p8Voltage;
	float fVgpVoltage;
	float fNtcResistanceOhm;
	uint16_t u16PullupResistanceOhm;

	/* 防御性检查：当前调用方不会传入空指针，但不能让错误参数继续访问内存。 */
	if(temp == 0)
	{
		return HSI_AFE_DVC1124_NTC_STATUS_INVALID;
	}
	*temp = INVALID_TEMPERATURE;

	/* 1.8V基准电压、修调上拉电阻和GP3电压任一寄存器读取失败，属于AFE通信异常。 */
	if(!hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0x0F), 2))
	{
		return HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT;
	}
	if(!hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(126), 1))
	{
		return HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT;
	}
	if(!hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0x15), 2))
	{
		return HSI_AFE_DVC1124_NTC_STATUS_IIC_FAULT;
	}

	fV1p8Voltage = ((gt_hsi_afe_dvc1124_regs.R15_16.V1P8_H << 8) | gt_hsi_afe_dvc1124_regs.R15_16.V1P8_L) * 0.1f;
	fVgpVoltage = ((gt_hsi_afe_dvc1124_regs.R17_28.VGP[2].VGP_H << 8) | gt_hsi_afe_dvc1124_regs.R17_28.VGP[2].VGP_L) * 0.1f;
	u16PullupResistanceOhm = (uint16_t)(gt_hsi_afe_dvc1124_regs.R126.F1RT * 25U + 6800U);

	/* VGP接近0表示NTC短路，VGP接近1.8V表示NTC断线。 */
	if((fV1p8Voltage <= 0.0f) || (fVgpVoltage <= 0.0f) || (fVgpVoltage >= fV1p8Voltage))
	{
		return HSI_AFE_DVC1124_NTC_STATUS_ELECTRICAL_FAULT;
	}

	/* NTC还串联了1K电阻，先由分压计算NTC阻值，单位为欧姆。 */
	fNtcResistanceOhm = ((fVgpVoltage / fV1p8Voltage * u16PullupResistanceOhm) / (1.0f - fVgpVoltage / fV1p8Voltage)) - 1000.0f;
	if((fNtcResistanceOhm <= 0.0f) || (fNtcResistanceOhm > 200000.0f))
	{
		return HSI_AFE_DVC1124_NTC_STATUS_ELECTRICAL_FAULT;
	}

	*temp = (int16_t)(hsi_ntc_get_temp(gt_hsi_ntc_map_2, (uint32_t)fNtcResistanceOhm) * 0.1f);
	return HSI_AFE_DVC1124_NTC_STATUS_VALID;

}
/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : Status 控制开关；
retval      : 成功/失败
function    : 控制充电MOS
**************************************************************************************/
int8_t hsi_afe_dvc1124_ChMosCtl(m_soft_i2c_t *i2c, uint16_t Status)
{
	if(Status == 1)
	gt_hsi_afe_dvc1124_regs.R81.CHGC = 0x03;
	else
	gt_hsi_afe_dvc1124_regs.R81.CHGC = 0x02;

	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(81), 1);

}
/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : Status 控制开关；
retval      : 成功/失败
function    : 控制放电MOS
**************************************************************************************/
int8_t hsi_afe_dvc1124_DchMosCtl(m_soft_i2c_t *i2c, uint16_t Status)
{
	if(Status == 1)
	gt_hsi_afe_dvc1124_regs.R81.DSGC = 0x03;
	else
	gt_hsi_afe_dvc1124_regs.R81.DSGC = 0x02;

	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(81), 1);

}

/**************************************************************************************
date        : 2025-2-14
Author      : xiaoai
parameter   : Status 控制开关；
retval      : 成功/失败
function    : 充放电MOS同时控制
**************************************************************************************/
/* 通过一次R81更新同时写入充放电MOS命令。 */
int8_t hsi_afe_dvc1124_MosCtl(m_soft_i2c_t *i2c, uint8_t CMOS, uint8_t DMOS)
{
	if(DMOS == 1)
	gt_hsi_afe_dvc1124_regs.R81.DSGC = 0x03;
	else
	gt_hsi_afe_dvc1124_regs.R81.DSGC = 0x02;//关闭，但是开启续流保护

	if(CMOS == 1)
		gt_hsi_afe_dvc1124_regs.R81.CHGC = 0x03;
	else
		gt_hsi_afe_dvc1124_regs.R81.CHGC = 0x02;//关闭，但是开启续流保护

	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(81), 1);

}
/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : mask均衡电芯序号；
retval      : 成功/失败
function    : 均衡功能
**************************************************************************************/
/* 将线性单体均衡位图按DVC1124的CB[0..2]寄存器顺序打包。 */
static void hsi_afe_dvc1124_pack_balance(uint32_t u32CellMask, volatile uint8_t pau8CbRegisters[3])
{
	u32CellMask &= hsi_afe_dvc1124_cell_mask(AFE_MAX_CELL_CNT);

	pau8CbRegisters[0] = (uint8_t)((u32CellMask >> 16U) & 0xFFU);
	pau8CbRegisters[1] = (uint8_t)((u32CellMask >> 8U) & 0xFFU);
	pau8CbRegisters[2] = (uint8_t)(u32CellMask & 0xFFU);
}

int8_t hsi_afe_dvc1124_Balance(m_soft_i2c_t *i2c, uint32_t u32CellMask)
{
	/* CB[0..2]依次保存单体17..24、9..16、1..8的均衡请求位。 */
	hsi_afe_dvc1124_pack_balance(u32CellMask, gt_hsi_afe_dvc1124_regs.R103_R105.CB);

	return hsi_afe_dvc1124_WriteRegs(i2c, AFE_ADDR_R(103),3);

}

/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   :
retval      : 成功/失败
function    : 获取故障状态
**************************************************************************************/
int8_t hsi_afe_dvc1124_GetStatus(m_soft_i2c_t *i2c)
{	
	return hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0), 1);
}

/**************************************************************************************
date        : 2025-03-06
Author      : xiaoai
parameter   :
retval      :  1（复位发生），0（正常），负数（I2C错误码）
function    : 检测设备是否因未配置短路阈值发生复位
****/
int8_t hsi_afe_dvc1124_CheckRST(m_soft_i2c_t *i2c)
{
	// 读取配置寄存器0x62
	int8_t s8ReadResult = hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0x62), 1);
	if (s8ReadResult != 1) return s8ReadResult;
	// 检查短路阈值配置位
	return (gt_hsi_afe_dvc1124_regs.R98.SCDT == 0) ? 1 : 0;
}
/***
date        : 2024-11-30
Author      : xiaoai
parameter   : cur电流值；
retval      : 电流值
function    : 获取电流采样值
**************************************************************************************/
/* 根据CC2的20位有符号寄存器值和采样电阻计算电流。 */
bool hsi_afe_dvc1124_CalcCurrent(m_soft_i2c_t *i2c, int32_t *cur, int32_t *reg)
{
	//	int32_t CC1Value;
	int32_t s32Cc2Value = 0;
	int32_t s32CurrentRegisterValue = 0;

	if((cur == 0) || (reg == 0))
	{
		return false;
	}

	if(hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(0x04), 3))
	{
		/**********************暂用寄存器CC1值**********************/
		//通过过滤器CC1的采样值来计算电流
	//		Reg_Value = (int16_t)((gt_hsi_afe_dvc1124_regs.R2_3.CC1_H<<8) | gt_hsi_afe_dvc1124_regs.R2_3.CC1_L);
	//		CC1Value = (int32_t)Reg_Value * 1000 * _lsbCC1 * 1000 / APP_AFE_CURRENT_SENSE_RESISTANCE_MR;

		//通过过滤器CC2的采样值来计算电流
		s32CurrentRegisterValue = ((gt_hsi_afe_dvc1124_regs.R4_6.CC2_H << 12) | (gt_hsi_afe_dvc1124_regs.R4_6.CC2_M << 4) | gt_hsi_afe_dvc1124_regs.R4_6.CC2_L);
		if(s32CurrentRegisterValue & 0x80000)
		{
			s32CurrentRegisterValue |= 0xFFF00000; //若为负数则补足32位补码
		}
		s32Cc2Value = s32CurrentRegisterValue * 1000 * _lsbCC2 * 1000 / APP_AFE_CURRENT_SENSE_RESISTANCE_MR;
		*reg = 0 - s32CurrentRegisterValue;
		*cur = 0 - s32Cc2Value;//返回电流值（单位mA）
		return true;
	}

	return false;
}

/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : ；
retval      : PACK采样值
function    : 获取AFE BAT采样值
**************************************************************************************/
void hsi_afe_dvc1124_CalcVBAT(m_soft_i2c_t *i2c, uint32_t *vbat)
{
	float fVbatVoltage = 0.0f;

	if(hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(7), 2))
	{
		fVbatVoltage = ((gt_hsi_afe_dvc1124_regs.R7_8.VBAT_H << 8) | gt_hsi_afe_dvc1124_regs.R7_8.VBAT_L) * _lsbVCELL * 10000 * 12.8f;
		*vbat = (uint32_t)(fVbatVoltage + 0.5f);//四舍五入
	}

}

/**************************************************************************************
date        : 2024-11-30
Author      : xiaoai
parameter   : ；
retval      : PACK采样值
function    : 获取AFE PACK采样值
**************************************************************************************/
void hsi_afe_dvc1124_CalcVPACK(m_soft_i2c_t *i2c, uint32_t *vpack)
{
	float fVpackVoltage = 0.0f;

	if(hsi_afe_dvc1124_ReadRegs(i2c, AFE_ADDR_R(9), 2))
	{
		fVpackVoltage = ((gt_hsi_afe_dvc1124_regs.R9_10.VPK_H << 8) | gt_hsi_afe_dvc1124_regs.R9_10.VPK_L) * _lsbVCELL * 10000 * 12.8f;
		*vpack = (uint32_t)(fVpackVoltage + 0.5f);//四舍五入
	}

}
