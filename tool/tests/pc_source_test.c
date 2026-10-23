#include <assert.h>
#include <stdint.h>

#include "../../app/N32G45x_KEIL/User_Src/Protocol/Comm/ducar_can_protocol.h"

int main(void)
{
    ducar_can_id_fields_t tCanIdFields = {0};

    tCanIdFields.u8Sa = DUCAR_CAN_PC_ADDRESS;
    assert(ducar_can_id_is_pc_source(&tCanIdFields));

    tCanIdFields.u8Sa = DUCAR_CAN_BMS_A_ADDRESS;
    assert(!ducar_can_id_is_pc_source(&tCanIdFields));

    assert(!ducar_can_id_is_pc_source(0));
    return 0;
}
