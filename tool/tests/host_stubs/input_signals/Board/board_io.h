#ifndef TEST_BOARD_IO_H
#define TEST_BOARD_IO_H

#include <stdint.h>

uint8_t board_io_load_detect_level(void);
uint8_t board_io_mcu_detd_level(void);
uint8_t board_io_mcu_detc_level(void);
uint8_t board_io_mcu_2nd_det_level(void);
uint8_t board_io_mcu_sw_level(void);

#endif
