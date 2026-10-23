/*
 * 文件作用：
 *   N32G45X片内Flash驱动实现，提供擦除、写入、读取和空白检查等抽象接口。
 *
 * 主要用途：
 *   1. 给IAP升级写APP2区域。
 *   2. 给LOG均衡擦写中间件提供底层Flash能力。
 *   3. 管理Flash地址范围、页擦除和写入对齐。
 *
 * 适用范围：
 *   只适用于当前N32G45X片内Flash。更换MCU时保留 d_flash.h，对本文件重新实现。
 *
 * 调用关系：
 *   IAP模块和 m_flash_wl 中间件调用本模块；本模块内部调用N32厂家Flash接口。
 *
 * 注意事项：
 *   Flash擦写会阻塞并影响实时性，业务层不能在中断中触发Flash操作。
 */
#include "Driver/d_flash.h"

#include "Driver/d_watchdog.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "n32g45x_flash.h"
#include "n32g45x_rcc.h"

#define D_FLASH_HSI_READY_TIMEOUT    ((uint32_t)100000UL)

static SemaphoreHandle_t s_d_flash_mutex = NULL;

bool d_flash_init(void)
{
    if (s_d_flash_mutex == NULL)
    {
        s_d_flash_mutex = xSemaphoreCreateRecursiveMutex();
    }

    return (s_d_flash_mutex != NULL);
}

bool d_flash_acquire(uint32_t timeout_ms)
{
    TickType_t timeout_ticks;

    if (s_d_flash_mutex == NULL)
    {
        return false;
    }

    /* 启动调度器前只有main单线程访问Flash，不阻塞也不尝试调用任务调度。 */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return true;
    }

    timeout_ticks = (timeout_ms == 0UL) ? 0U : pdMS_TO_TICKS(timeout_ms);
    if ((timeout_ms != 0UL) && (timeout_ticks == 0U))
    {
        timeout_ticks = 1U;
    }

    return (xSemaphoreTakeRecursive(s_d_flash_mutex, timeout_ticks) == pdTRUE);
}

void d_flash_release(void)
{
    if ((s_d_flash_mutex != NULL) &&
        (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED))
    {
        (void)xSemaphoreGiveRecursive(s_d_flash_mutex);
    }
}

bool d_flash_is_page_aligned(uint32_t addr)
{
    return ((addr % D_FLASH_PAGE_SIZE) == 0UL);
}

bool d_flash_is_word_aligned(uint32_t addr)
{
    return ((addr & 0x03UL) == 0UL);
}

bool d_flash_check_range(uint32_t addr, uint32_t len)
{
    if ((addr < D_FLASH_START_ADDR) || (addr >= D_FLASH_END_ADDR))
    {
        return false;
    }

    if ((len == 0UL) || ((addr + len) > D_FLASH_END_ADDR) || ((addr + len) <= addr))
    {
        return false;
    }

    return true;
}

bool d_flash_is_blank(uint32_t addr, uint32_t len)
{
    uint32_t u32ByteOffset;
    bool bFlashBlank = true;

    if (!d_flash_check_range(addr, len))
    {
        return false;
    }

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return false;
    }

    for (u32ByteOffset = 0UL; u32ByteOffset < len; u32ByteOffset++)
    {
        if (*((volatile const uint8_t *)(addr + u32ByteOffset)) != 0xFFU)
        {
            bFlashBlank = false;
            break;
        }
    }

    d_flash_release();
    return bFlashBlank;
}

static bool d_flash_prepare(void)
{
    uint32_t u32TimeoutCount = D_FLASH_HSI_READY_TIMEOUT;

    RCC_EnableHsi(ENABLE);
    while ((RCC_GetFlagStatus(RCC_FLAG_HSIRD) == RESET) && (u32TimeoutCount > 0UL))
    {
        d_watchdog_feed();
        u32TimeoutCount--;
    }

    if (RCC_GetFlagStatus(RCC_FLAG_HSIRD) == RESET)
    {
        return false;
    }

    FLASH_Unlock();
    FLASH_SetSMPSELStatus(FLASH_SMPSEL_SMP1);
    FLASH_ClearFlag(FLASH_STS_CLRFLAG);
    return true;
}

bool d_flash_erase_page(uint32_t page_addr)
{
    FLASH_STS status;

    if ((!d_flash_is_page_aligned(page_addr)) || (!d_flash_check_range(page_addr, D_FLASH_PAGE_SIZE)))
    {
        return false;
    }

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return false;
    }

    if (!d_flash_prepare())
    {
        d_flash_release();
        return false;
    }

    status = FLASH_EraseOnePage(page_addr);
    d_watchdog_feed();
    FLASH_Lock();
    d_flash_release();
    return (status == FLASH_COMPL);
}

bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t u32ByteOffset;
    uint32_t u32ProgramLength;
    uint32_t u32ProgramWord;
    uint8_t *pu8ProgramWordBytes;
    uint8_t u8ByteIndex;
    FLASH_STS status;

    if ((data == 0) || (!d_flash_is_word_aligned(addr)) || (len == 0UL) ||
        (len > 0xFFFFFFFCUL))
    {
        return false;
    }

    u32ProgramLength = (len + 3UL) & ~3UL;
    if (!d_flash_check_range(addr, u32ProgramLength))
    {
        return false;
    }

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return false;
    }

    if (!d_flash_prepare())
    {
        d_flash_release();
        return false;
    }

    for (u32ByteOffset = 0UL; u32ByteOffset < len; u32ByteOffset += 4UL)
    {
        d_watchdog_feed();
        u32ProgramWord = D_FLASH_EMPTY_WORD;
        pu8ProgramWordBytes = (uint8_t *)&u32ProgramWord;

        for (u8ByteIndex = 0U; (u8ByteIndex < 4U) && ((u32ByteOffset + u8ByteIndex) < len); u8ByteIndex++)
        {
            pu8ProgramWordBytes[u8ByteIndex] = data[u32ByteOffset + u8ByteIndex];
        }

        if (*((volatile const uint32_t *)(addr + u32ByteOffset)) != D_FLASH_EMPTY_WORD)
        {
            FLASH_Lock();
            d_flash_release();
            return false;
        }

        status = FLASH_ProgramWord(addr + u32ByteOffset, u32ProgramWord);
        if ((status != FLASH_COMPL) || (*((volatile const uint32_t *)(addr + u32ByteOffset)) != u32ProgramWord))
        {
            FLASH_Lock();
            d_flash_release();
            return false;
        }
    }

    FLASH_Lock();
    d_flash_release();
    return true;
}

