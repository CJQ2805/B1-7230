#include "../../app/N32G45x_KEIL/User_Src/App/app_h_power_manage.h"

/*
 * 此契约测试防止CAN PF=09重新绕过功率管理器而直接写全局MOS状态。
 * 若远程MOS公开入口或三种合法选择被删除，目标编译必须失败。
 */
int app_h_power_remote_mos_contract_test(void)
{
    app_result_t eResult;

    eResult = app_h_power_manage_set_remote_mos_command(
        APP_H_POWER_MOS_SELECT_PRECHARGE,
        APP_MOS_ON,
        1U);
    if (eResult != APP_RESULT_OK)
    {
        return 1;
    }

    eResult = app_h_power_manage_set_remote_mos_command(
        APP_H_POWER_MOS_SELECT_CHARGE,
        APP_MOS_OFF,
        1U);
    if (eResult != APP_RESULT_OK)
    {
        return 2;
    }

    eResult = app_h_power_manage_set_remote_mos_command(
        APP_H_POWER_MOS_SELECT_DISCHARGE,
        APP_MOS_ON,
        1U);
    return (eResult == APP_RESULT_OK) ? 0 : 3;
}
