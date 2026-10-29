#ifndef __APP_FLASH_MAP_H__
#define __APP_FLASH_MAP_H__

#include <stdint.h>

/*
 * 文件作用：
 *   统一记录APP工程使用到的内部Flash地址规划，避免各模块自己随手写固定地址。
 *
 * 适用范围：
 *   本文件只描述当前N32G45X项目的Flash分区。BOOT、APP、LOG、APP database的边界必须和
 *   BOOT工程、打包工具、Keil链接地址保持一致。
 *
 * 注意事项：
 *   1. N32G45X内部Flash页大小为2K，所有需要擦写的数据区都按页对齐。
 *   2. APP database当前不做A/B轮换，Page0合并保存校准参数和SN，SOC独占Page1。
 *   3. 新增持久化数据时，必须先在这里分配页，再到业务模块中引用对应宏。
 */

/* 芯片内部Flash总范围：512K。 */
#define APP_FLASH_MAP_FLASH_START_ADDR          ((uint32_t)0x08000000UL)
#define APP_FLASH_MAP_FLASH_END_ADDR            ((uint32_t)0x08080000UL)
#define APP_FLASH_MAP_FLASH_PAGE_SIZE           ((uint32_t)0x00000800UL)

/* BOOT代码区：0x08000000~0x0800F7FF，共62K。 */
#define APP_FLASH_MAP_BOOT_START_ADDR           ((uint32_t)0x08000000UL)
#define APP_FLASH_MAP_BOOT_END_ADDR             ((uint32_t)0x0800F800UL)

/* BOOT数据库区：0x0800F800~0x0800FFFF，共2K，用于升级元数据和状态标志。 */
#define APP_FLASH_MAP_BOOT_DB_START_ADDR        ((uint32_t)0x0800F800UL)
#define APP_FLASH_MAP_BOOT_DB_END_ADDR          ((uint32_t)0x08010000UL)
#define APP_FLASH_MAP_BOOT_UPGRADE_INFO_ADDR    (APP_FLASH_MAP_BOOT_DB_START_ADDR)
#define APP_FLASH_MAP_BOOT_UPGRADE_INFO_MAGIC   ((uint32_t)0x4950414DUL)
#define APP_FLASH_MAP_BOOT_UPGRADE_INFO_SIZE    ((uint32_t)16UL)
#define APP_FLASH_MAP_BOOT_UPGRADE_FLAG_ADDR    (APP_FLASH_MAP_BOOT_DB_END_ADDR - 4UL)

/* APP1代码区：0x08010000~0x0803EFFF，共188K。 */
#define APP_FLASH_MAP_APP1_START_ADDR           ((uint32_t)0x08010000UL)
#define APP_FLASH_MAP_APP1_END_ADDR             ((uint32_t)0x0803F000UL)
#define APP_FLASH_MAP_APP_CODE_SIZE             ((uint32_t)0x0002F000UL)

/* APP database：0x0803F000~0x08040FFF，共8K，按固定页存储低频持久化数据。 */
#define APP_FLASH_MAP_DB_START_ADDR             ((uint32_t)0x0803F000UL)
#define APP_FLASH_MAP_DB_END_ADDR               ((uint32_t)0x08041000UL)
#define APP_FLASH_MAP_DB_SIZE                   ((uint32_t)0x00002000UL)

/* APP database Page0：各数据区以2字节ASCII标识开头，后面紧跟实际数据。 */
#define APP_FLASH_MAP_DB_PARAM_ADDR             ((uint32_t)0x0803F000UL)
#define APP_FLASH_MAP_DB_PARAM_CURRENT_CALIB_OFFSET ((uint32_t)0x00000000UL)
#define APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET ((uint32_t)0x0000000EUL)
#define APP_FLASH_MAP_DB_PARAM_SN_OFFSET        ((uint32_t)0x00000054UL)
#define APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET ((uint32_t)0x00000088UL)

/* 小端Flash中可直接按两个ASCII字符识别：UC、VC、SN、PM(产品模式)、SC(SOC)、PF、FP(下载指纹)。 */
#define APP_FLASH_TAG_CURRENT_CALIB             ((uint16_t)0x4355U) /* 字节：55 43，即“UC”。 */
#define APP_FLASH_TAG_VOLTAGE_CALIB             ((uint16_t)0x4356U) /* 字节：56 43，即“VC”。 */
#define APP_FLASH_TAG_MANUFACTURE_SN            ((uint16_t)0x4E53U) /* 字节：53 4E，即“SN”。 */
#define APP_FLASH_TAG_PRODUCT_MODE              ((uint16_t)0x4D50U) /* 字节：50 4D，即“PM”。 */
#define APP_FLASH_TAG_SOC                       ((uint16_t)0x4353U) /* 字节：53 43，即“SC”。 */
#define APP_FLASH_TAG_PF                        ((uint16_t)0x4650U) /* 字节：50 46，即“PF”。 */
#define APP_FLASH_TAG_UDS_DOWNLOAD_FINGERPRINT  ((uint16_t)0x5046U) /* 字节：46 50，即“FP”。 */

/* APP database Page1：SOC运行状态固定保存页。 */
#define APP_FLASH_MAP_DB_SOC_ADDR               ((uint32_t)0x0803F800UL)

/* APP database Page2：PF锁存状态，shutdown、复位前、升级前保存，上电恢复。 */
#define APP_FLASH_MAP_DB_PF_ADDR                ((uint32_t)0x08040000UL)

/*
 * APP database Page3：UDS下载指纹专用页，0x08040800~0x08040FFF。
 * 记录仅含“FP”两字节标识和客户规范定义的8字节指纹：
 * 年(2字节BCD)、月(BCD)、日(BCD)、下载设备编号(4字节ASCII)。
 * 不引入版本号、长度、完成标志或额外CRC。
 */
#define APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_ADDR ((uint32_t)0x08040800UL)
#define APP_FLASH_MAP_DB_UDS_DOWNLOAD_FINGERPRINT_SIZE ((uint32_t)8UL)

/* APP2代码区：0x08041000~0x0806FFFF，共188K，升级备份区。 */
#define APP_FLASH_MAP_APP2_START_ADDR           ((uint32_t)0x08041000UL)
#define APP_FLASH_MAP_APP2_END_ADDR             ((uint32_t)0x08070000UL)

/* LOG数据库区：0x08070000~0x0807FFFF，共64K。 */
#define APP_FLASH_MAP_LOG_DB_START_ADDR         ((uint32_t)0x08070000UL)
#define APP_FLASH_MAP_LOG_DB_END_ADDR           ((uint32_t)0x08080000UL)
#define APP_FLASH_MAP_LOG_DB_SIZE               ((uint32_t)0x00010000UL)
#define APP_FLASH_MAP_LOG_RTD_START_ADDR        (APP_FLASH_MAP_LOG_DB_START_ADDR + APP_FLASH_MAP_FLASH_PAGE_SIZE)

/* 实时LOG均衡擦写区：保留首个Flash页后，再使用24页（48KB）保存运行日志。 */
#define APP_FLASH_MAP_LOG_RTD_PAGE_COUNT_VALUE  24UL
#define APP_FLASH_MAP_LOG_RTD_PAGE_COUNT        ((uint16_t)APP_FLASH_MAP_LOG_RTD_PAGE_COUNT_VALUE)

/* 末尾两页预留，不得用作电流校准存储。 */
#define APP_FLASH_MAP_RESERVED_PAGE_A_ADDR      ((uint32_t)0x0807F000UL)
#define APP_FLASH_MAP_RESERVED_PAGE_B_ADDR      ((uint32_t)0x0807F800UL)

#if (0x08070800UL + (APP_FLASH_MAP_LOG_RTD_PAGE_COUNT_VALUE * 0x800UL)) > 0x0807F000UL
#error "Realtime LOG overlaps reserved pages"
#endif

/* 防止后续调整实时LOG页数时越过内部Flash末地址。 */
#if (0x08070800UL + (APP_FLASH_MAP_LOG_RTD_PAGE_COUNT_VALUE * 0x800UL)) > 0x08080000UL
#error "Realtime LOG Flash partition overlaps the end of internal Flash"
#endif

#endif

