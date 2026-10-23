#ifndef __APP_PARAM_API_H__
#define __APP_PARAM_API_H__

#include <stdbool.h>
#include <stdint.h>

/* 固定的制造SN协议/存储长度，属于参数模块接口，不是客户项目配置项。 */
#define APP_BMS_MANUFACTURE_SN_MAX_LEN 50U
#define APP_PARAM_PRODUCT_MODE_PRODUCTION 0U
#define APP_PARAM_PRODUCT_MODE_SHIPPING   1U

/* 由app_param.c提供唯一实体，BMS任务和SOC算法共同读取同一张OCV表。 */
extern const uint16_t g_app_param_soc_cell_ocv_table[101];

/* 初始化运行时参数：分别恢复模式、SN和电压校准偏差。 */
void app_param_init(void);

/* 在APP参数共享页内更新任意字节区间并保留页中其他参数。 */
bool app_param_flash_page_update(uint32_t offset, const uint8_t *data, uint32_t len);

/* 产品模式独立于SN：0为生产/测试，1为出货；写入后掉电保持。 */
uint8_t app_param_get_product_mode(void);
bool app_param_set_product_mode(uint8_t mode);

/* 写入不带结尾'\0'的制造SN ASCII内容，长度范围1~50字节。 */
bool app_param_set_manufacture_sn(const uint8_t *data, uint8_t len);
/* 清除成品SN，不改变产品模式。 */
bool app_param_clear_manufacture_sn(void);

/* 设置、读取或应用单体电压校准偏差，偏差单位mV。 */
bool app_param_set_cell_voltage_offset(uint8_t cell_index, int16_t offset_mv);
int16_t app_param_get_cell_voltage_offset(uint8_t cell_index);
uint16_t app_param_apply_cell_voltage_offset(uint8_t cell_index, uint16_t measured_mv);

/* 读取运行时项目身份与版本字符串。 */
const char *app_param_get_software_pn(void);
const char *app_param_get_manufacture_sn(void);
const char *app_param_get_bat_type(void);
const char *app_param_get_hw_version(void);
const char *app_param_get_sw_version(void);
const char *app_param_get_package_t_version(void);
const char *app_param_get_package_v_version(void);

#endif
