#include "App/app_param.h"

#ifdef APP_LOW_POWER_IDLE_CURRENT_MA
#error "Low-power current idle must use the configured zero deadband, not another threshold"
#endif
#ifdef APP_PARAM_ZS_B1_TX_WAKE_CURRENT_MA
#error "The derived Zongshen wake threshold must not be exposed as a project parameter"
#endif

int main(void)
{
    return 0;
}
