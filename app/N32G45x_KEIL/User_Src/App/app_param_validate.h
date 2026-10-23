#ifndef __APP_PARAM_VALIDATE_H__
#define __APP_PARAM_VALIDATE_H__

#include "app_param.h"

/* 参数合法性检查属于编译约束，单独放置以保持项目参数清单简洁。 */
#if (APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS == 0UL)
#error "APP_PARAM_ZS_B1_TX_IDLE_TIMEOUT_MS must be greater than 0"
#endif

#if (APP_PARAM_UDS_ENABLE != 0U) && (APP_PARAM_UDS_ENABLE != 1U)
#error "APP_PARAM_UDS_ENABLE must be 0U or 1U"
#endif

#if (APP_BMS_MAX_CELL_NUM < 1U) || (APP_BMS_MAX_CELL_NUM > 24U)
#error "APP_BMS_MAX_CELL_NUM must be between 1 and 24 for DVC1124"
#endif
#if (APP_BMS_CELL_COUNT < 1U) || (APP_BMS_CELL_COUNT > APP_BMS_MAX_CELL_NUM)
#error "APP_BMS_CELL_COUNT must be between 1 and APP_BMS_MAX_CELL_NUM"
#endif
#if (APP_BMS_AFE_SCAN_CELL_COUNT < APP_BMS_CELL_COUNT) || \
    (APP_BMS_AFE_SCAN_CELL_COUNT > APP_BMS_MAX_CELL_NUM)
#error "APP_BMS_AFE_SCAN_CELL_COUNT must cover configured cells and fit the AFE buffer"
#endif
#if (APP_BMS_AFE_TOP_CELL_INDEX >= APP_BMS_AFE_SCAN_CELL_COUNT) || \
    ((APP_BMS_CELL_COUNT > 1U) && (APP_BMS_AFE_TOP_CELL_INDEX < (APP_BMS_CELL_COUNT - 1U)))
#error "APP_BMS_AFE_TOP_CELL_INDEX must be in scan range and not overlap prior logical cells"
#endif

#endif
