#ifndef __D_FLASH_H__
#define __D_FLASH_H__

#include <stdbool.h>
#include <stdint.h>

#define D_FLASH_START_ADDR    ((uint32_t)0x08000000UL)
#define D_FLASH_END_ADDR      ((uint32_t)0x08080000UL)
#define D_FLASH_PAGE_SIZE     ((uint32_t)0x00000800UL)
#define D_FLASH_EMPTY_WORD    ((uint32_t)0xFFFFFFFFUL)
#define D_FLASH_MUTEX_TIMEOUT_MS ((uint32_t)50UL)

/* 初始化并启用片内Flash资源守卫。必须在创建业务任务前调用。 */
bool d_flash_init(void);
/* 获取Flash资源；支持同一任务递归获取，超时返回false。 */
bool d_flash_acquire(uint32_t timeout_ms);
/* 释放Flash资源；必须与d_flash_acquire成对调用。 */
void d_flash_release(void);

/* 判断地址是否按Flash页对齐。 */
bool d_flash_is_page_aligned(uint32_t addr);
/* 判断地址是否按32位写入要求对齐。 */
bool d_flash_is_word_aligned(uint32_t addr);
/* 判断一段地址是否位于当前芯片Flash范围内。 */
bool d_flash_check_range(uint32_t addr, uint32_t len);
/* 判断指定Flash范围是否全部保持擦除态。 */
bool d_flash_is_blank(uint32_t addr, uint32_t len);
/* 擦除一页Flash。 */
bool d_flash_erase_page(uint32_t page_addr);
/* 按word写入Flash，尾部不足4字节时用0xFF补齐。 */
bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len);

#endif
