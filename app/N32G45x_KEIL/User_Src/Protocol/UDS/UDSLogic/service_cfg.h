/******************************************************************************
* 文件名称: service_cfg.h
* 内容摘要: UDS 诊断请求服务配置头文件
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/

#ifndef _SERVICE_CFG_H_
#define _SERVICE_CFG_H_


#define SID_NUM       5      // APP仅保留进入BOOT前所需的UDS服务

#define SID_10        (0x10) /* SessionControl */
#define SID_11        (0x11) /* ECUReset */
#define SID_14        (0x14) /* ClearDTC */
#define SID_18        (0x18) /* KWPReadDTC */
#define SID_19        (0x19) /* ReadDTC */
#define SID_22        (0x22) /* ReadID */
#define SID_27        (0x27) /* SecurityAccess */
#define SID_2E        (0x2E) /* WriteID */
#define SID_2F        (0x2F) /* InputOutputControlID */
#define SID_28        (0x28) /* CommunicationControl */
#define SID_31        (0x31) /* RoutineControl */
#define SID_3E        (0x3E) /* TesterPresent */
#define SID_85        (0x85) /* ControlDTCSetting */
#define SID_34        (0x34) /* RequestDownload */
#define SID_36        (0x36) /* TransferData */
#define SID_37        (0x37) /* RequestTransferExit */


#endif
/****************EOF****************/
