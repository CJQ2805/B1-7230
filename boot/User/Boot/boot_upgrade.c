#include "boot_upgrade.h"

#include "Boot/boot_app.h"
#include "Boot/boot_config.h"
#include "Hal_Driver/boot_crc.h"
#include "Hal_Driver/boot_flash.h"
#include "Hal_Driver/boot_watchdog.h"
#include <stddef.h>
#include <string.h>

#define BOOT_UPGRADE_COPY_CHUNK_SIZE    ((uint32_t)256UL)

static uint8_t s_boot_copy_buffer[BOOT_UPGRADE_COPY_CHUNK_SIZE]; /* 搬运固件时使用的RAM缓冲区。 */

typedef struct
{
    uint32_t magic;                 /* 元数据有效标识。 */
    uint32_t image_size;            /* 待提交镜像的实际长度。 */
    uint32_t image_crc32;           /* 待提交镜像的整包CRC32。 */
    uint32_t record_crc32;          /* 对前面三个字段计算的元数据CRC32。 */
} boot_upgrade_move_info_t;

uint32_t boot_upgrade_get_flag(void)
{
    return *((volatile const uint32_t *)BOOT_UPGRADE_FLAG_ADDR);
}

static bool boot_upgrade_flag_is_supported(uint32_t flag)
{
    return ((flag == BOOT_UPGRADE_FLAG_DEFAULT) ||
            (flag == BOOT_UPGRADE_FLAG_APP_VALID) ||
            (flag == BOOT_UPGRADE_FLAG_WAIT_PACKAGE) ||
            (flag == BOOT_UPGRADE_FLAG_NEED_MOVE) ||
            (flag == BOOT_UPGRADE_FLAG_APP1_READY));
}

static bool boot_upgrade_erase_db(void)
{
    return boot_flash_erase_page(BOOT_DB_START_ADDR);
}

static bool boot_upgrade_write_flag(uint32_t flag)
{
    if (flag == BOOT_UPGRADE_FLAG_DEFAULT)
    {
        return true;
    }

    return boot_flash_write(BOOT_UPGRADE_FLAG_ADDR,
                            (const uint8_t *)&flag,
                            (uint32_t)sizeof(flag));
}

/*
 * 写入升级状态标志。
 * 标志位放在BOOT数据页最后4字节；由于Flash只能从1写成0，状态变化前需要擦除整页。
 */
bool boot_upgrade_set_flag(uint32_t flag)
{
    if (!boot_upgrade_flag_is_supported(flag))
    {
        return false;
    }

    if (!boot_upgrade_erase_db())
    {
        return false;
    }

    return boot_upgrade_write_flag(flag);
}

/* 在同一页中准备“待搬运”记录：先写元数据，最后写CCCC状态字。 */
bool boot_upgrade_prepare_move(uint32_t image_size, uint32_t image_crc32)
{
    boot_upgrade_move_info_t tMoveInfo;

    if ((image_size < 8UL) || (image_size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    tMoveInfo.magic = BOOT_UPGRADE_INFO_MAGIC;
    tMoveInfo.image_size = image_size;
    tMoveInfo.image_crc32 = image_crc32;
    tMoveInfo.record_crc32 = boot_crc32(
        (const uint8_t *)&tMoveInfo,
        (uint32_t)offsetof(boot_upgrade_move_info_t, record_crc32));

    if (!boot_upgrade_erase_db())
    {
        return false;
    }

    if (!boot_flash_write(BOOT_UPGRADE_INFO_ADDR,
                          (const uint8_t *)&tMoveInfo,
                          BOOT_UPGRADE_INFO_SIZE))
    {
        return false;
    }

    return boot_upgrade_write_flag(BOOT_UPGRADE_FLAG_NEED_MOVE);
}

/* 记录已经直接写入APP1的镜像；BOOT重启后用长度和CRC确认再启动。 */
bool boot_upgrade_prepare_app1(uint32_t image_size, uint32_t image_crc32)
{
    boot_upgrade_move_info_t tImageInfo;

    if ((image_size < 8UL) || (image_size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    tImageInfo.magic = BOOT_UPGRADE_INFO_MAGIC;
    tImageInfo.image_size = image_size;
    tImageInfo.image_crc32 = image_crc32;
    tImageInfo.record_crc32 = boot_crc32(
        (const uint8_t *)&tImageInfo,
        (uint32_t)offsetof(boot_upgrade_move_info_t, record_crc32));

    if (!boot_upgrade_erase_db() ||
        !boot_flash_write(BOOT_UPGRADE_INFO_ADDR,
                          (const uint8_t *)&tImageInfo,
                          BOOT_UPGRADE_INFO_SIZE))
    {
        return false;
    }

    return boot_upgrade_write_flag(BOOT_UPGRADE_FLAG_APP1_READY);
}

/* 读取并校验APP写入的待搬运镜像长度和CRC。 */
bool boot_upgrade_get_move_info(uint32_t *image_size, uint32_t *image_crc32)
{
    const boot_upgrade_move_info_t *ptMoveInfo;
    uint32_t u32RecordCrc32;

    if ((image_size == 0) || (image_crc32 == 0))
    {
        return false;
    }

    ptMoveInfo = (const boot_upgrade_move_info_t *)BOOT_UPGRADE_INFO_ADDR;
    if ((ptMoveInfo->magic != BOOT_UPGRADE_INFO_MAGIC) ||
        (ptMoveInfo->image_size < 8UL) ||
        (ptMoveInfo->image_size > BOOT_APP_CODE_SIZE))
    {
        return false;
    }

    u32RecordCrc32 = boot_crc32(
        (const uint8_t *)ptMoveInfo,
        (uint32_t)offsetof(boot_upgrade_move_info_t, record_crc32));
    if (u32RecordCrc32 != ptMoveInfo->record_crc32)
    {
        return false;
    }

    *image_size = ptMoveInfo->image_size;
    *image_crc32 = ptMoveInfo->image_crc32;
    return true;
}

/* 断电重启后确认APP1向量表及本次升级镜像CRC均正确。 */
bool boot_upgrade_app1_ready_is_valid(void)
{
    uint32_t image_size;
    uint32_t image_crc32;

    return boot_upgrade_get_move_info(&image_size, &image_crc32) &&
           boot_app_app1_is_valid() &&
           (boot_crc32((const uint8_t *)BOOT_APP1_START_ADDR, image_size) == image_crc32);
}

static bool boot_upgrade_copy_one_page(uint32_t src_page_addr, uint32_t dst_page_addr)
{
    uint32_t offset;
    uint32_t copy_len;

    if (!boot_flash_erase_page(dst_page_addr))
    {
        return false;
    }

    for (offset = 0UL; offset < BOOT_FLASH_PAGE_SIZE; offset += BOOT_UPGRADE_COPY_CHUNK_SIZE)
    {
        boot_watchdog_feed();
        copy_len = BOOT_UPGRADE_COPY_CHUNK_SIZE;
        if ((offset + copy_len) > BOOT_FLASH_PAGE_SIZE)
        {
            copy_len = BOOT_FLASH_PAGE_SIZE - offset;
        }

        memcpy(s_boot_copy_buffer, (const void *)(src_page_addr + offset), copy_len);
        if (!boot_flash_write(dst_page_addr + offset, s_boot_copy_buffer, copy_len))
        {
            return false;
        }
    }

    return true;
}

/*
 * 把APP2备份区的有效固件搬运到APP1运行区。
 * 搬运前重新校验APP2长度和CRC，搬运页数按实际镜像长度向上取整。
 */
bool boot_upgrade_move_backup_to_app1(void)
{
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t copy_size;
    uint32_t offset;

    if (!boot_upgrade_get_move_info(&image_size, &image_crc32) ||
        !boot_app_backup_is_valid() ||
        (boot_crc32((const uint8_t *)BOOT_APP2_START_ADDR, image_size) != image_crc32))
    {
        return false;
    }

    copy_size = ((image_size + BOOT_FLASH_PAGE_SIZE - 1UL) / BOOT_FLASH_PAGE_SIZE) * BOOT_FLASH_PAGE_SIZE;
    for (offset = 0UL; offset < copy_size; offset += BOOT_FLASH_PAGE_SIZE)
    {
        boot_watchdog_feed();
        if (!boot_upgrade_copy_one_page(BOOT_APP2_START_ADDR + offset, BOOT_APP1_START_ADDR + offset))
        {
            return false;
        }
    }

    return boot_app_app1_is_valid() &&
           (boot_crc32((const uint8_t *)BOOT_APP1_START_ADDR, image_size) == image_crc32);
}
