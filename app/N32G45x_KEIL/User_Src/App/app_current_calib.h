#ifndef __APP_CURRENT_CALIB_H__
#define __APP_CURRENT_CALIB_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * 文件作用：
 *   管理主回路电流的统一K/B校准参数，并把校准后的电流送回AFE业务数据链路。
 *
 * 主要用途：
 *   1. 使用 Y = KX + B 修正AFE原始电流，充电和放电共用一组K/B。
 *   2. 通过AFE_Task的10ms周期累计5个AFE新样本，避免通信命令处理等待采样完成。
 *   3. 将统一K/B和零点原始值保存到当前Flash映射预留的CAL2双页日志，掉电后继续使用；兼容读取旧Page0记录。
 *
 * 适用范围：
 *   仅适用于当前APP工程的主回路电流校准。模块只依赖hsi_afe公开的电流数据和
 *   d_flash抽象接口，不直接参与AFE寄存器访问，也不修改IAP流程。
 *
 * 调用关系：
 *   AFE初始化阶段调用app_current_calib_init()；AFE_Task以10ms周期调用app_current_calib_task()，
 *   模块通过current_update_seq只处理AFE产生的新样本。
 *   通信模块通过app_current_calib_submit()提交上位机给出的实际电流值，
 *   通过app_current_calib_get()读取充放电共用的K/B。
 *
 * 协议约定：
 *   0x71写入一个实际校准点，DATA为有符号int32_t，单位mA，小端格式；
 *   0mA可选地采集零点；未校0A时默认原始零点R0=0。非零数值是一个已知校准点，
 *   正值可在充电时使用，负值可在放电时使用，两种方向计算并更新同一组K/B。
 *   0x72读取唯一的一组K/B，K以百万倍整数K_PPM发送，B以mA发送，固定8字节不变。
 */

/* K_PPM = K * 1000000，默认K=1.0。 */
#define APP_CURRENT_CALIB_K_DEFAULT_PPM       ((uint32_t)1000000UL)

/* 每个校准点采集5个AFE新样本；AFE电流刷新约700ms一次，总采样时间约3.5s。 */
#define APP_CURRENT_CALIB_SAMPLE_COUNT        ((uint8_t)5U)

typedef enum
{
    APP_CURRENT_CALIB_CAL2_OK = 0,
    APP_CURRENT_CALIB_CAL2_SPACING,
    APP_CURRENT_CALIB_CAL2_FIT
} app_current_calib_cal2_fit_result_t;

/* 两点拟合纯计算：原始/参考均为有符号mA，成功时返回共享K_PPM/B。 */
app_current_calib_cal2_fit_result_t app_current_calib_cal2_fit(
    int32_t raw1_ma, int32_t actual1_ma, int32_t raw2_ma, int32_t actual2_ma,
    uint32_t *k_ppm, int32_t *b_ma);

typedef struct
{
    uint8_t status;
    uint8_t operation;
    uint8_t sequence;
    uint8_t destination;
    uint16_t session;
} app_current_calib_cal2_completion_t;

/* CAN 1ms时基、E700 DLC8操作和异步完成通知。 */
void app_current_calib_cal2_advance_ms(uint32_t elapsed_ms);
uint8_t app_current_calib_cal2_request(uint8_t operation, uint8_t sequence,
                                       uint16_t session, int32_t reference_ma,
                                       uint8_t source);
bool app_current_calib_cal2_take_completion(app_current_calib_cal2_completion_t *completion);
bool app_current_calib_cal2_is_active(void);
bool app_current_calib_cal2_has_completion(void);

/* 提交给0x71的实际电流点；0mA校零可选，返回false表示当前忙。 */
bool app_current_calib_submit(int32_t actual_current_ma);

/* 初始化默认参数并从APP database读取已经保存的K/B。 */
void app_current_calib_init(void);

/* AFE_Task的10ms任务入口：处理新样本采集、统一K/B计算、Flash保存和校准电流输出。 */
void app_current_calib_task(void);

/* 读取充放电共用的K/B，K以K_PPM格式返回，B单位mA。 */
bool app_current_calib_get(uint32_t *k_ppm, int32_t *b_ma);

/* 查询是否正在等待校准采样完成。 */
bool app_current_calib_is_busy(void);

/* 以下三个接口是无硬件依赖的计算接口，便于主机测试和公式回归验证。 */
bool app_current_calib_calculate_high(int32_t zero_raw_ma,
                                      int32_t high_raw_ma,
                                      int32_t actual_high_ma,
                                      uint32_t *k_ppm,
                                      int32_t *b_ma);
bool app_current_calib_calculate_zero_bias(uint32_t k_ppm,
                                           int32_t zero_raw_ma,
                                           int32_t *b_ma);
int32_t app_current_calib_calculate_output(uint32_t k_ppm,
                                           int32_t b_ma,
                                           int32_t raw_ma);

#endif
