#ifndef __APP_HW_APPLY_H__
#define __APP_HW_APPLY_H__

#include "App/app_bms_types.h"

/* 应用层AFE清除请求掩码，具体寄存器编码由本文件对应的适配实现转换。 */
#define APP_HW_AFE_CLEAR_SCD          0x01U
#define APP_HW_AFE_CLEAR_OCC2         0x02U
#define APP_HW_AFE_CLEAR_OCD2         0x04U
#define APP_HW_AFE_CLEAR_OCC1         0x08U
#define APP_HW_AFE_CLEAR_OCD1         0x10U
#define APP_HW_AFE_CLEAR_CUV          0x20U
#define APP_HW_AFE_CLEAR_COV          0x40U
#define APP_HW_AFE_CLEAR_IWF          0x80U
#define APP_HW_AFE_CLEAR_NONE         0x00U

/* 将业务模块输出的硬件控制命令真正执行到当前项目硬件；board_power_port 也复用这条路径。 */
void app_hw_apply_control(const app_bms_control_t *control);

/* 低功耗前硬件动作统一入口，后续逐步收敛低功耗模块直接硬件操作。 */
void app_hw_apply_prepare_sleep(void);

/* 低功耗唤醒后的硬件恢复统一入口。 */
void app_hw_apply_restore_after_wakeup(void);

/* 控制DCDC_EN电源使能，供普通关机命令使用。 */
void app_hw_apply_dcdc_enable(uint8_t enable);

/* 控制负载检测电路使能脚，供故障模块通过适配层访问板级动作。 */
void app_hw_apply_load_detect_enable(uint8_t enable_level);

/* MCU_LD_EN未使能时，LD输入不具备判定负载状态的条件。 */
#define APP_HW_LOAD_DETECT_INVALID       2U

/* 返回负载移除状态：1=移除，0=在位，2=检测未使能/无效。 */
uint8_t app_hw_apply_load_detect_level(void);

/* 清除AFE放电过流和短路硬件故障，避免故障模块直接访问AFE寄存器。 */
void app_hw_apply_clear_afe_dsg_oc_short_fault(void);

/* 清除AFE充电过流硬件故障，供充电过流恢复逻辑调用。 */
void app_hw_apply_clear_afe_chg_oc_fault(void);

/* 清除DVC1124指定的硬件故障；本地AFE状态缓存等待下一次状态读取更新。 */
uint8_t app_hw_apply_clear_afe_fault(uint8_t clear_mask);

/* 清除DVC1124当前所有可清除硬件故障；返回1表示清除命令已成功写入AFE。 */
uint8_t app_hw_apply_clear_all_afe_fault(void);

/* 控制二级熔断输出脚，供功率路径模块通过适配层访问板级动作。 */
void app_hw_apply_efuse_set(uint8_t enable);

#endif
