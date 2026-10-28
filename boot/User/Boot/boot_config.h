#ifndef __BOOT_CONFIG_H__
#define __BOOT_CONFIG_H__

#include <stdint.h>

/* N32G45X内部Flash总大小：512K，地址范围0x08000000~0x08080000。 */
#define BOOT_FLASH_START_ADDR          ((uint32_t)0x08000000UL)
#define BOOT_FLASH_END_ADDR            ((uint32_t)0x08080000UL)
#define BOOT_FLASH_PAGE_SIZE           ((uint32_t)0x00000800UL)

/* Boot代码区：0x08000000~0x0800F800，共62K。 */
#define BOOT_CODE_START_ADDR           ((uint32_t)0x08000000UL)
#define BOOT_CODE_SIZE                 ((uint32_t)0x0000F800UL)
#define BOOT_CODE_END_ADDR             (BOOT_CODE_START_ADDR + BOOT_CODE_SIZE)

/* Boot数据库区：0x0800F800~0x08010000，共2K，正好1个Flash页。 */
#define BOOT_DB_START_ADDR             ((uint32_t)0x0800F800UL)
#define BOOT_DB_SIZE                   ((uint32_t)0x00000800UL)
#define BOOT_DB_END_ADDR               (BOOT_DB_START_ADDR + BOOT_DB_SIZE)
#define BOOT_UPGRADE_INFO_ADDR         (BOOT_DB_START_ADDR)
#define BOOT_UPGRADE_INFO_MAGIC        ((uint32_t)0x4950414DUL)
#define BOOT_UPGRADE_INFO_SIZE         ((uint32_t)16UL)
#define BOOT_UPGRADE_FLAG_ADDR         (BOOT_DB_END_ADDR - 4UL)

/* 升级状态标志位：BBBB等待接收，CCCC表示APP2待搬运，DDDD表示APP1待校验启动。 */
#define BOOT_UPGRADE_FLAG_DEFAULT     ((uint32_t)0xFFFFFFFFUL)
#define BOOT_UPGRADE_FLAG_APP_VALID   ((uint32_t)0xAAAAAAAAUL)
#define BOOT_UPGRADE_FLAG_WAIT_PACKAGE ((uint32_t)0xBBBBBBBBUL)
#define BOOT_UPGRADE_FLAG_NEED_MOVE    ((uint32_t)0xCCCCCCCCUL)
#define BOOT_UPGRADE_FLAG_APP1_READY   ((uint32_t)0xDDDDDDDDUL)

/* BOOT侧本机软件品号；A000000000作为未定义品号时的强制升级通配品号。 */
#define BOOT_PARAM_SOFTWARE_PN          "A800000002"

/* App1代码区：0x08010000~0x0803F000，共188K。 */
#define BOOT_APP1_START_ADDR           ((uint32_t)0x08010000UL)
#define BOOT_APP_CODE_SIZE             ((uint32_t)0x0002F000UL)
#define BOOT_APP1_END_ADDR             (BOOT_APP1_START_ADDR + BOOT_APP_CODE_SIZE)

/* App数据库区：0x0803F000~0x08041000，共8K。 */
#define BOOT_APP_DB_START_ADDR         ((uint32_t)0x0803F000UL)
#define BOOT_APP_DB_SIZE               ((uint32_t)0x00002000UL)
#define BOOT_APP_DB_END_ADDR           (BOOT_APP_DB_START_ADDR + BOOT_APP_DB_SIZE)

/* App2代码区：0x08041000~0x08070000，共188K。 */
#define BOOT_APP2_START_ADDR           ((uint32_t)0x08041000UL)
#define BOOT_APP2_END_ADDR             (BOOT_APP2_START_ADDR + BOOT_APP_CODE_SIZE)

/* Log数据库区：0x08070000~0x08080000，共64K。 */
#define BOOT_LOG_DB_START_ADDR         ((uint32_t)0x08070000UL)
#define BOOT_LOG_DB_SIZE               ((uint32_t)0x00010000UL)
#define BOOT_LOG_DB_END_ADDR           (BOOT_LOG_DB_START_ADDR + BOOT_LOG_DB_SIZE)

/* SRAM范围，用于判断App向量表里的初始栈指针是否合法。 */
#define BOOT_SRAM_START_ADDR           ((uint32_t)0x20000000UL)
#define BOOT_SRAM_SIZE                 ((uint32_t)0x00024000UL)
#define BOOT_SRAM_END_ADDR             (BOOT_SRAM_START_ADDR + BOOT_SRAM_SIZE)

/* Boot上电后预留的等待时间，当前主流程通过升级标志决定是否停留BOOT。 */
#define BOOT_WAIT_APP_MS               ((uint32_t)1000UL)

/* Boot调试串口：沿用APP当前USART2映射，PB4=TX，PB5=RX，默认9600。 */
#define BOOT_UART_BAUDRATE             ((uint32_t)9600UL)

#endif

