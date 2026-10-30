/******************************************************************************
* 文件名称: uds_port.c
* 内容摘要: UDS 协议栈移植接口
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/

#include "uds_port.h"

#include "App/app_can.h"
#include "Driver/d_can.h"
#include "Driver/d_system.h"
#include "Driver/d_timer.h"
#include "uds_service.h"
#include "uds_tp.h"
#include "Task/bms_task.h"

static volatile uint8_t s_uds_reset_after_tx;

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
void uds_recv_frame(uint32_t id, uint8_t* frame_buf, uint8_t frame_dlc)
{
    if (REQUEST_ID == id)
    {
        /* 宗申物理UDS请求。 */
        uds_tp_recv_frame(0, frame_buf, frame_dlc);
    }
    else if (FUNCTIONAL_REQUEST_ID == id)
    {
        /* 仅服务表显式支持的功能请求会被处理，TP层不发送功能响应。 */
        uds_tp_recv_frame(1, frame_buf, frame_dlc);
    }
}


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
int uds_send_frame(uint32_t response_id, uint8_t* frame_buf, uint8_t frame_dlc)
{
    /* CAN IAP期间仅允许升级帧占用总线，UDS发送交由传输层超时收敛。 */
    if (app_can_iap_is_busy())
    {
        return -1;
    }

    return d_can_send(&can0, response_id, D_CAN_ID_STANDARD, frame_buf, frame_dlc) ? 0 : -1;
}

void uds_request_reset_after_tx(void)
{
    s_uds_reset_after_tx = 1U;
}

bool uds_port_is_reset_pending(void)
{
    return (s_uds_reset_after_tx != 0U);
}

uint32_t uds_port_get_time_ms(void)
{
    return d_timer_get_ms();
}


/******************************************************************************
* 函数名称: void uds_init(void)
* 功能说明: UDS 初始化
* 输入参数: 无
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void uds_init(void)
{
    service_init();
}


/******************************************************************************
* 函数名称: void uds_1ms_task(void)
* 功能说明: UDS 周期任务
* 输入参数: 无
* 输出参数: 无
* 函数返回: 无
* 其它说明: 该函数需要被 1ms 周期调用
******************************************************************************/
void uds_1ms_task(void)
{
    network_task();
    service_task();

    /* 不在服务回调栈内立即复位；先让肯定响应完成CAN发送。 */
    if ((s_uds_reset_after_tx != 0U) &&
        !app_can_iap_is_busy() &&
        d_can_is_tx_idle(&can0))
    {
        /* 编程会话/镜像交接先尝试保存SOC，失败也不挂住升级复位。 */
        (void)app_bms_soc_save_for_transition();
        s_uds_reset_after_tx = 0U;
        d_system_reset();
    }
}
