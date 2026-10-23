#ifndef __DUCAR_CAN_BEHAVIOR_TEST_APP_PARAM_H__
#define __DUCAR_CAN_BEHAVIOR_TEST_APP_PARAM_H__

/*
 * Keep the production configuration intact, but make the two F220
 * controller-undervoltage thresholds distinct for the host behavior test.
 * This proves that the protocol selects the low-temperature threshold.
 */
#include "../../../../app/N32G45x_KEIL/User_Src/App/app_param.h"

#undef CONTROLLER_UNDER_VOLTAGE_PROTECT
#undef CONTROLLER_UNDER_VOLTAGE_PROTECT_MINUS_0_TMEP
#define CONTROLLER_UNDER_VOLTAGE_PROTECT             60000U
#define CONTROLLER_UNDER_VOLTAGE_PROTECT_MINUS_0_TMEP 58000U

#endif
