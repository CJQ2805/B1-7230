#ifndef _M_FLASH_WL_H_
#define _M_FLASH_WL_H_

#include <stdbool.h>
#include <stdint.h>

#define Module_Major_Version_M_Flash_WL    0
#define Module_Minor_Version_M_Flash_WL    1

#define M_FLASH_WL_FLASH_START_ADDR        ((uint32_t)0x08000000U)
#define M_FLASH_WL_FLASH_END_ADDR          ((uint32_t)0x08080000U)
#define M_FLASH_WL_PAGE_SIZE               ((uint32_t)2048U)

#define M_FLASH_WL_PAGE_HEADER_SIZE        ((uint16_t)64U)
#define M_FLASH_WL_RECORD_HEADER_SIZE      ((uint16_t)16U)
#define M_FLASH_WL_RECORD_COMMIT_SIZE      ((uint16_t)4U)
#define M_FLASH_WL_MIN_RECORD_SIZE         ((uint16_t)(M_FLASH_WL_RECORD_HEADER_SIZE + M_FLASH_WL_RECORD_COMMIT_SIZE + 4U))

typedef enum
{
    M_FLASH_WL_OK = 0,
    M_FLASH_WL_ERR_PARAM,
    M_FLASH_WL_ERR_ALIGN,
    M_FLASH_WL_ERR_RANGE,
    M_FLASH_WL_ERR_NOT_INIT,
    M_FLASH_WL_ERR_EMPTY,
    M_FLASH_WL_ERR_NO_SPACE,
    M_FLASH_WL_ERR_CRC,
    M_FLASH_WL_ERR_FLASH
} m_flash_wl_status_t;

/* 均衡擦写分区配置。
 * record_size 是每条记录占用的 Flash 总长度，已经包含底层记录头和提交标志。
 */
typedef struct
{
    uint32_t start_addr;
    uint16_t page_count;
    uint16_t record_size;
    uint8_t area_id;
} m_flash_wl_cfg_t;

/* 分区运行状态由模块维护，上层只需要定义变量并传入接口。 */
typedef struct
{
    m_flash_wl_cfg_t cfg;
    uint32_t write_addr;
    uint32_t latest_addr;
    uint32_t page_seq;
    uint32_t next_record_seq;
    uint32_t valid_record_count;
    uint16_t current_page;
    uint16_t records_per_page;
    uint16_t payload_size;
    bool initialized;
} m_flash_wl_area_t;

typedef struct
{
    uint32_t start_addr;
    uint32_t write_addr;
    uint32_t latest_addr;
    uint32_t page_seq;
    uint32_t next_record_seq;
    uint32_t valid_record_count;
    uint16_t current_page;
    uint16_t page_count;
    uint16_t record_size;
    uint16_t records_per_page;
    uint16_t payload_size;
    bool initialized;
} m_flash_wl_info_t;

/* 初始化分区并扫描已有记录；空白分区只写第一页页头，不会整区擦除。 */
m_flash_wl_status_t m_flash_wl_init(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg);
/* 格式化整个分区，会擦除该分区内所有页。 */
m_flash_wl_status_t m_flash_wl_format(m_flash_wl_area_t *area, const m_flash_wl_cfg_t *cfg);
/* 追加一条记录；写入长度不能超过 m_flash_wl_get_payload_size() 返回值。 */
m_flash_wl_status_t m_flash_wl_append(m_flash_wl_area_t *area, const uint8_t *data, uint16_t len);
/* 读取最新一条有效记录。 */
m_flash_wl_status_t m_flash_wl_read_latest(const m_flash_wl_area_t *area, uint8_t *buffer,
                                           uint16_t buffer_len, uint16_t *out_len, uint32_t *out_seq);
/* 按有效记录索引读取；index从1开始，1表示最旧记录，valid_record_count表示最新记录。 */
m_flash_wl_status_t m_flash_wl_read_by_index(const m_flash_wl_area_t *area, uint16_t index,
                                             uint8_t *buffer, uint16_t buffer_len,
                                             uint16_t *out_len, uint32_t *out_seq);
/* 获取单条记录可用的业务数据长度。 */
uint16_t m_flash_wl_get_payload_size(const m_flash_wl_area_t *area);
/* 拷贝当前分区状态，方便上层调试和显示。 */
m_flash_wl_status_t m_flash_wl_get_info(const m_flash_wl_area_t *area, m_flash_wl_info_t *info);

#endif
