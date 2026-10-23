/******************************************************************************
* 文件名称: uds_port.h
* 内容摘要: UDS 协议栈移植接口头文件
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/

#ifndef _UDS_PORT_H_
#define _UDS_PORT_H_

#include <stdint.h>
#include "Protocol/Comm/ducar_can_protocol.h"

#define FRAME_SIZE      8               // 帧长度

/* 宗申刷写工具使用固定11位CAN地址，不随DUCAR PACK源地址变化。 */
#define REQUEST_ID              0x7A0U
#define RESPONSE_ID             0x7A8U
#define FUNCTIONAL_REQUEST_ID   0x7DFU

#define UDS_RX_MAX      1024            // 接收缓冲区长度 --最大支持 4095 字节，可根据实际需求改动，以免资源浪费
#define UDS_TX_MAX      128             // 发送缓冲区长度 --最大支持 4095 字节，可根据实际需求改动，以免资源浪费

#define PADDING_VAL     0xFF            // DLC固定8字节；未使用的数据位按协议填充0xFF


/******************************************************************************
* 函数名称: void uds_recv_frame(uint32_t id, uint8_t* frame_buf, uint8_t frame_dlc)
* 功能说明: 接收到一帧报文
* 输入参数: uint32_t    id              --消息帧 ID
    　　　　uint8_t*    frame_buf       --接收报文帧数据首地址
    　　　　uint8_t     frame_dlc       --接收报文帧数据长度
* 输出参数: 无
* 函数返回: 无
* 其它说明: frame_dlc 长度必须等于 FRAME_SIZE，否则会被判断为无效帧
******************************************************************************/
void uds_recv_frame(uint32_t id, uint8_t* frame_buf, uint8_t frame_dlc);


/******************************************************************************
* 函数名称: void uds_send_frame(uint32_t id, uint8_t* frame_buf, uint8_t frame_dlc)
* 功能说明: 发送一帧报文
* 输入参数: uint8_t     response_id     --应答 ID
    　　　　uint8_t*    frame_buf       --发送报文帧数据首地址
    　　　　uint8_t     frame_dlc       --发送报文帧数据长度
* 输出参数: 无
* 函数返回: 无
* 其它说明: frame_dlc 长度应当等于 FRAME_SIZE
******************************************************************************/
int uds_send_frame(uint32_t response_id, uint8_t* frame_buf, uint8_t frame_dlc);


/******************************************************************************
* 函数名称: void uds_init(void)
* 功能说明: UDS 初始化
* 输入参数: 无
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void uds_init(void);


/******************************************************************************
* 函数名称: void uds_1ms_task(void)
* 功能说明: UDS 周期任务
* 输入参数: 无
* 输出参数: 无
* 函数返回: 无
* 其它说明: 该函数需要被 1ms 周期调用
******************************************************************************/
void uds_1ms_task(void);

/* 在11 01肯定响应提交后，请求1ms任务等待CAN发送邮箱空闲再复位。 */
void uds_request_reset_after_tx(void);

/* 平台适配层毫秒计时；供安全访问随机种子使用。 */
uint32_t uds_port_get_time_ms(void);

#endif

