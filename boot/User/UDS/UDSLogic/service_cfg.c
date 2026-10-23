#include "service_cfg.h"

#include "SID10_SessionControl.h"
#include "SID11_EcuReset.h"
#include "SID22_ReadDataByIdentifier.h"
#include "SID27_SecurityAccess.h"
#include "SID28_CommunicationControl.h"
#include "SID2E_WriteDataByIdentifier.h"
#include "SID31_RoutineControl.h"
#include "SID34_RequestDownload.h"
#include "SID36_TransferData.h"
#include "SID37_RequestTransferExit.h"
#include "SID3E_TesterPresent.h"
#include "SID85_ControlDTCSetting.h"
#include "uds_service.h"

const uds_service_t uds_service_list[SID_NUM] =
{
    {SID_10, service_10_SessionControl,        service_10_check_len,         TRUE, TRUE, TRUE, TRUE, TRUE, UDS_SA_NON},
    {SID_11, service_11_EcuReset,              service_11_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_NON},
    {SID_22, service_22_ReadDataByIdentifier,  service_22_check_len,         TRUE, TRUE, TRUE, FALSE, FALSE, UDS_SA_NON},
    {SID_27, service_27_SecurityAccess,        service_27_check_len,         FALSE, TRUE, TRUE, FALSE, FALSE, UDS_SA_NON},
    {SID_2E, service_2E_WriteDataByIdentifier, service_2E_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_LV2},
    {SID_28, service_28_CommunicationControl,  service_28_check_len,         TRUE, FALSE, TRUE, TRUE, TRUE, UDS_SA_NON},
    {SID_31, service_31_RoutineControl,        service_31_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_LV2},
    {SID_34, service_34_RequestDownload,       service_34_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_LV2},
    {SID_36, service_36_TransferData,           service_36_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_LV2},
    {SID_37, service_37_RequestTransferExit,    service_37_check_len,         FALSE, TRUE, FALSE, FALSE, FALSE, UDS_SA_LV2},
    {SID_3E, service_3E_TesterPresent,          service_3E_check_len,         TRUE, TRUE, TRUE, TRUE, TRUE, UDS_SA_NON},
    {SID_85, service_85_ControlDTCSetting,     service_85_check_len,         TRUE, FALSE, TRUE, TRUE, TRUE, UDS_SA_NON},
};
