#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "App/app_bms_types.h"
#include "App/app_bms_fault.h"

static void test_fault_snapshot_image_preserves_named_bits_and_raw_words(void)
{
    app_bms_fault_image_t image;
    t_bms_fault_flag source;

    memset(&image, 0, sizeof(image));
    memset(&source, 0, sizeof(source));

    source.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov = 1U;
    source.udischarge_fault_B.tdischarge_fault_B.b1predischarge_lock = 1U;
    image.detail = source;
    image.charge_pro = 1U;

    assert(image.detail.ucharge_fault_A.tcharge_fault_A.b1cell_war_cov == 1U);
    assert(image.detail.ucharge_fault_A.u16fault_A == 0x0001U);
    assert(image.detail.udischarge_fault_B.tdischarge_fault_B.b1predischarge_lock == 1U);
    assert(image.detail.udischarge_fault_B.u16fault_B == 0x0100U);
    assert(image.charge_pro == 1U);
}

int main(void)
{
    test_fault_snapshot_image_preserves_named_bits_and_raw_words();
    return 0;
}
