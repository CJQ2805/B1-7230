/*
 * Target contract test for the normal DUCAR CAN protocol entry.
 *
 * This test is compiled by ArmClang together with ducar_can_protocol.c.
 * It intentionally uses only the public protocol contract: PC source
 * recognition must depend solely on the J1939 source address.
 */
#include "../../app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.h"

int ducar_can_protocol_contract_test(void)
{
    ducar_can_id_fields_t tCanIdFields = {0};

    tCanIdFields.u8Sa = DUCAR_CAN_PC_ADDRESS;
    if (!ducar_can_id_is_pc_source(&tCanIdFields))
    {
        return 1;
    }

    tCanIdFields.u8Sa = DUCAR_CAN_BMS_A_ADDRESS;
    if (ducar_can_id_is_pc_source(&tCanIdFields))
    {
        return 2;
    }

    return 0;
}
