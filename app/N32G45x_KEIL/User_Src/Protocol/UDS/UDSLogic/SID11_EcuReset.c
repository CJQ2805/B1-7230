/******************************************************************************
* 文件名称: SID11_EcuReset.c
* 内容摘要: ECU 重启
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/

#include "SID11_EcuReset.h"
#include "service_cfg.h"
#include "uds_service.h"
#include "uds_port.h"
#include "uds_upgrade.h"


typedef enum __UDS_RESET_T_
{
	UDS_RESET_NONE = 0,
	UDS_RESET_HARD,
	UDS_RESET_KEYOFFON,
	UDS_RESET_SOFT
}uds_reset_t;


/******************************************************************************
* 函数名称: bool_t service_11_check_len(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 检查 11 服务数据长度是否合法
* 输入参数: uint16_t msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: TRUE: 合法; FALSE: 非法
* 其它说明: 无
******************************************************************************/
bool_t service_11_check_len(const uint8_t* msg_buf, uint16_t msg_dlc)
{
	bool_t ret = FALSE;
	
	(void)msg_buf;
	if(2 == msg_dlc)
		ret = TRUE;

	return ret;
}

/******************************************************************************
* 函数名称: void service_11_EcuReset(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 11 服务 - ECU 重启
* 输入参数: uint8_t*    msg_buf         --数据首地址
    　　　　uint8_t     msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void service_11_EcuReset(const uint8_t* msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
    uint8_t rsp_buf[2];

    (void)msg_dlc;
    subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);

    /* 客户协议通过11 03软复位结束编程；其它复位子功能不属于升级流程。 */
    if (subfunction != UDS_RESET_SOFT)
    {
        uds_negative_rsp(SID_11, NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }
    if (!uds_upgrade_image_verified())
    {
        uds_negative_rsp(SID_11, NRC_CONDITIONS_NOT_CORRECT);
        return;
    }
    if (!uds_upgrade_mark_app1_ready())
    {
        uds_negative_rsp(SID_11, NRC_GENERAL_PROGRAMMING_FAILURE);
        return;
    }

    rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_11);
    rsp_buf[1] = UDS_RESET_SOFT;

    /* 只有51 03成功提交到CAN邮箱，才允许1ms任务等待发送完成后复位。 */
    if (uds_positive_rsp(rsp_buf, sizeof(rsp_buf)) == 0)
    {
        uds_request_reset_after_tx();
    }
}


/****************EOF****************/
