/******************************************************************************
* 文件名称: SID28_CommunicationControl.h
* 内容摘要: 通讯控制头文件
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/


#ifndef _SID28_COMMUNICATION_CONTROL_H_
#define _SID28_COMMUNICATION_CONTROL_H_


#include <stdint.h>
#include "uds_type.h"

typedef enum __UDS_CC_MODE__
{
	UDS_CC_MODE_RX_TX = 0,	 	// 使能接收和发送
	UDS_CC_MODE_RX_NO,			// 使能接收、禁止发送
	UDS_CC_MODE_NO_TX,			// 禁止接收、使能发送
	UDS_CC_MODE_NO_NO			// 禁止接收、禁止发送
}uds_cc_mode;


/******************************************************************************
* 函数名称: void service_28_CommunicationControl(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 28 服务 - 通讯控制
* 输入参数: uint8_t*    msg_buf         --数据首地址
    　　　　uint8_t     msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void service_28_CommunicationControl(const uint8_t* msg_buf, uint16_t msg_dlc);

/* Accessors used by APP CAN dispatch; UDS standard frames remain enabled. */
bool_t uds_normal_receive_enabled(void);
bool_t uds_normal_transmit_enabled(void);
void service_28_reset(void);
/* Fixed five-minute ceiling; poll from the active UDS task and RTC sleep wake path. */
bool_t service_28_timeout_poll(void);
void service_28_timeout_task_1ms(void);
void service_28_on_s3_timeout(void);


/******************************************************************************
* 函数名称: bool_t service_28_check_len(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 检查 28 服务数据长度是否合法
* 输入参数: uint16_t msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: TRUE: 合法; FALSE: 非法
* 其它说明: 无
******************************************************************************/
bool_t service_28_check_len(const uint8_t* msg_buf, uint16_t msg_dlc);


#endif


/****************EOF****************/
