#include "boot_flash.h"

#include "Boot/boot_config.h"
#include "Hal_Driver/boot_watchdog.h"
#include "n32g45x_flash.h"
#include "n32g45x_rcc.h"

#define BOOT_FLASH_EMPTY_WORD          ((uint32_t)0xFFFFFFFFUL)
#define BOOT_FLASH_HSI_READY_TIMEOUT   ((uint32_t)100000UL)

/* 判断地址是否4字节对齐，N32G45X内部Flash按word编程。 */
static bool boot_flash_is_word_aligned(uint32_t value)
{
    return ((value & 0x03UL) == 0UL);
}

/* 判断地址是否Flash页对齐。 */
static bool boot_flash_is_page_aligned(uint32_t addr)
{
    return ((addr % BOOT_FLASH_PAGE_SIZE) == 0UL);
}

/* 检查地址范围是否落在片内Flash内。 */
static bool boot_flash_check_range(uint32_t addr, uint32_t len)
{
    if ((addr < BOOT_FLASH_START_ADDR) || (addr >= BOOT_FLASH_END_ADDR))
    {
        return false;
    }

    if ((len == 0UL) || ((addr + len) > BOOT_FLASH_END_ADDR) || ((addr + len) <= addr))
    {
        return false;
    }

    return true;
}

/* Flash擦写前打开HSI并解锁Flash控制器。 */
static bool boot_flash_prepare(void)
{
    uint32_t timeout = BOOT_FLASH_HSI_READY_TIMEOUT;

    RCC_EnableHsi(ENABLE); /* 厂家库要求内部Flash编程前HSI处于使能状态。 */
    while ((RCC_GetFlagStatus(RCC_FLAG_HSIRD) == RESET) && (timeout > 0UL))
    {
        timeout--;
    }

    if (timeout == 0UL)
    {
        return false;
    }

    FLASH_Unlock();
    FLASH_SetSMPSELStatus(FLASH_SMPSEL_SMP1);
    FLASH_ClearFlag(FLASH_STS_CLRFLAG);
    return true;
}

/* 擦除一个Flash页。 */
bool boot_flash_erase_page(uint32_t page_addr)
{
    FLASH_STS status;

    if ((!boot_flash_is_page_aligned(page_addr)) || (!boot_flash_check_range(page_addr, BOOT_FLASH_PAGE_SIZE)))
    {
        return false;
    }

    if (!boot_flash_prepare())
    {
        return false;
    }

    status = FLASH_EraseOnePage(page_addr);
    boot_watchdog_feed();
    FLASH_Lock();

    return (status == FLASH_COMPL);
}

/* 写入任意长度数据，尾部不足4字节时按0xFF补齐。 */
bool boot_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t offset;
    uint32_t word;
    uint8_t *word_byte;
    uint8_t i;
    FLASH_STS status;

    if ((data == 0) || (!boot_flash_is_word_aligned(addr)) || (!boot_flash_check_range(addr, len)))
    {
        return false;
    }

    if (!boot_flash_prepare())
    {
        return false;
    }

    for (offset = 0UL; offset < len; offset += 4UL)
    {
        boot_watchdog_feed();
        word = BOOT_FLASH_EMPTY_WORD;
        word_byte = (uint8_t *)&word;

        for (i = 0U; (i < 4U) && ((offset + i) < len); i++)
        {
            word_byte[i] = data[offset + i];
        }

        if (*((volatile const uint32_t *)(addr + offset)) != BOOT_FLASH_EMPTY_WORD)
        {
            FLASH_Lock();
            return false;
        }

        status = FLASH_ProgramWord(addr + offset, word);
        if ((status != FLASH_COMPL) || (*((volatile const uint32_t *)(addr + offset)) != word))
        {
            FLASH_Lock();
            return false;
        }
    }

    FLASH_Lock();
    return true;
}

/* 从Flash读数据到RAM。 */
bool boot_flash_read(uint32_t addr, uint8_t *data, uint32_t len)
{
    uint32_t i;

    if ((data == 0) || (!boot_flash_check_range(addr, len)))
    {
        return false;
    }

    for (i = 0UL; i < len; i++)
    {
        data[i] = *((volatile const uint8_t *)(addr + i));
    }

    return true;
}

/* 判断指定Flash范围是否仍然是擦除态。 */
bool boot_flash_is_blank(uint32_t addr, uint32_t len)
{
    uint32_t offset;

    if ((!boot_flash_is_word_aligned(addr)) || ((len % 4UL) != 0UL) || (!boot_flash_check_range(addr, len)))
    {
        return false;
    }

    for (offset = 0UL; offset < len; offset += 4UL)
    {
        if (*((volatile const uint32_t *)(addr + offset)) != BOOT_FLASH_EMPTY_WORD)
        {
            return false;
        }
    }

    return true;
}

