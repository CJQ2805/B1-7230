/******************************************************************************
* 文件名称: service_cfg.c
* 内容摘要: UDS 诊断请求服务配置
******************************************************************************/

#include "service_cfg.h"
#include "uds_service.h"

#include "SID27_SecurityAccess.h"
#include "SID10_SessionControl.h"
#include "SID22_ReadDataByIdentifier.h"
#include "SID28_CommunicationControl.h"
#include "SID85_ControlDTCSetting.h"

/* APP只负责升级前握手和切换BOOT；下载、校验、复位升级均由BOOT处理。 */
const uds_service_t uds_service_list[SID_NUM] =
{
    /* SID   handler                          length check                 default  program  extended  functional  suppress  security */
    {SID_10, service_10_SessionControl,       service_10_check_len,         TRUE,    TRUE,    TRUE,     TRUE,        TRUE,     UDS_SA_NON},
    {SID_22, service_22_ReadDataByIdentifier, service_22_check_len,         TRUE,    TRUE,    TRUE,     FALSE,       FALSE,    UDS_SA_NON},
    {SID_27, service_27_SecurityAccess,       service_27_check_len,         FALSE,   TRUE,    TRUE,     FALSE,       FALSE,    UDS_SA_NON},
    {SID_28, service_28_CommunicationControl, service_28_check_len,         TRUE,    FALSE,   TRUE,     TRUE,         TRUE,     UDS_SA_NON},
    {SID_85, service_85_ControlDTCSetting,    service_85_check_len,         TRUE,    FALSE,   TRUE,     TRUE,         TRUE,     UDS_SA_NON},
};

/****************EOF****************/
