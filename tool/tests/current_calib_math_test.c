#include <assert.h>
#include <stdint.h>

#include "../../app/N32G45x_KEIL/User_Src/App/app_current_calib.h"

int main(void)
{
    uint32_t k_ppm;
    int32_t b_ma;

    assert(app_current_calib_calculate_output(1000000U, 0, 1234) == 1234);
    assert(app_current_calib_calculate_high(100, 10100, 10000, &k_ppm, &b_ma));
    assert(k_ppm == 1000000U);
    assert(b_ma == -100);
    assert(app_current_calib_calculate_high(0, 20000, 10000, &k_ppm, &b_ma));
    assert(k_ppm == 500000U);
    assert(b_ma == 0);
    assert(app_current_calib_calculate_high(0, -20000, -10000, &k_ppm, &b_ma));
    assert(k_ppm == 500000U);
    assert(b_ma == 0);
    assert(!app_current_calib_calculate_high(0, 20000, -10000, &k_ppm, &b_ma));
    assert(app_current_calib_calculate_zero_bias(500000U, 100, &b_ma));
    assert(b_ma == -50);
    assert(app_current_calib_calculate_output(500000U, -50, 20100) == 10000);
    assert(app_current_calib_calculate_output(500000U, -50, -19900) == -10000);
    /* CAL2 has one signed K/B for both current directions. */
    assert(app_current_calib_cal2_fit(1124, 1000, 21124, 21000,
                                      &k_ppm, &b_ma) == APP_CURRENT_CALIB_CAL2_OK);
    assert(k_ppm == 1000000U && b_ma == -124);
    assert(app_current_calib_cal2_fit(-10000, -9000, 10000, 11000,
                                      &k_ppm, &b_ma) == APP_CURRENT_CALIB_CAL2_OK);
    assert(k_ppm == 1000000U && b_ma == 1000);
    assert(app_current_calib_cal2_fit(10000, 10000, 11999, 12000,
                                      &k_ppm, &b_ma) == APP_CURRENT_CALIB_CAL2_SPACING);
    assert(app_current_calib_cal2_fit(10000, 10000, 20000, 7000,
                                      &k_ppm, &b_ma) == APP_CURRENT_CALIB_CAL2_FIT);
    return 0;
}
