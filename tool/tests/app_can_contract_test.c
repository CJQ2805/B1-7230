#include <stdbool.h>

#include "../../app/N32G45x_KEIL/User_Src/App/app_can.h"

/*
 * Contract break caught by this test:
 * removing or changing any of the three APP CAN scheduling entry points.
 * The firmware build supplies the real implementation; this file is not
 * included in the Keil target.
 */
int app_can_contract_test(void)
{
    bool bCanReady;

    bCanReady = app_can_init();
    CAN_ReceiveTask();
    CAN_TransmitTask();

    return bCanReady ? 0 : 1;
}
