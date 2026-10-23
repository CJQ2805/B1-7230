#ifndef TEST_HSI_AFE_H
#define TEST_HSI_AFE_H

#include <stdint.h>

typedef struct
{
    uint8_t data_ready;
    uint8_t status_valid;
    struct { uint8_t data[3]; } status;
    int32_t curr_reg;
    int32_t curr_raw;
    int32_t curr_calibrated;
    int32_t curr_real;
    uint32_t current_update_seq;
} hsi_afe_t;

extern hsi_afe_t gt_hsi_afe;

#endif
