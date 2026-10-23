#ifndef HSI_NTC_H
#define HSI_NTC_H

#include <stdint.h>

#define Module_Major_Version_HSI_NTC    2
#define Module_Minor_Version_HSI_NTC    1

/* 阻值-温度表项：x为欧姆，y为0.1摄氏度。 */
typedef struct
{
signed int x;
signed int y;
}hsi_ntc_map_t;

/* 保留两张表，是为了兼容原AFE代码的两个温度通道。 */
extern hsi_ntc_map_t gt_hsi_ntc_map_1[];
extern hsi_ntc_map_t gt_hsi_ntc_map_2[];

/* 通过带边界钳位的线性插值，将NTC阻值转换为温度。 */
int16_t hsi_ntc_get_temp(hsi_ntc_map_t *map, uint32_t res);

#endif
