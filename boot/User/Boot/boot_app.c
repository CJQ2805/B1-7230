#include "boot_app.h"

#include "Boot/boot_config.h"
#include "n32g45x.h"
#include "n32g45x_tim.h"
#include "n32g45x_usart.h"

volatile uint32_t g_boot_app_jump_base;
volatile uint32_t g_boot_app_jump_stack;
volatile uint32_t g_boot_app_jump_reset;
volatile uint32_t g_boot_app_jump_vtor;
volatile uint32_t g_boot_app_jump_stage;

/* 判断APP向量表中的栈顶地址是否落在SRAM范围内。 */
static bool boot_app_stack_is_valid(uint32_t stack_addr)
{
    return ((stack_addr >= BOOT_SRAM_START_ADDR) &&
            (stack_addr <= BOOT_SRAM_END_ADDR) &&
            ((stack_addr & 0x03UL) == 0UL));
}

/* 判断APP复位入口是否落在APP1运行地址范围内，并且保持Thumb入口最低位为1。 */
static bool boot_app_reset_is_valid(uint32_t reset_addr)
{
    return ((reset_addr >= BOOT_APP1_START_ADDR) &&
            (reset_addr < BOOT_APP1_END_ADDR) &&
            ((reset_addr & 0x01UL) != 0UL));
}

/*
 * 判断一份APP镜像是否有效。
 * image_addr是镜像当前存放地址：APP1运行区或APP2备份区。
 * 镜像本身按APP1地址链接，所以复位入口始终按APP1范围判断。
 */
static bool boot_app_image_is_valid(uint32_t image_addr)
{
    uint32_t stack_addr;
    uint32_t reset_addr;

    stack_addr = *((volatile const uint32_t *)image_addr);
    reset_addr = *((volatile const uint32_t *)(image_addr + 4UL));

    if (!boot_app_stack_is_valid(stack_addr))
    {
        return false;
    }

    if (!boot_app_reset_is_valid(reset_addr))
    {
        return false;
    }

    return true;
}

/* APP1是运行区，默认上电和搬运完成后都只跳转到这里。 */
bool boot_app_app1_is_valid(void)
{
    return boot_app_image_is_valid(BOOT_APP1_START_ADDR);
}

/* APP2是备份区，里面存放的仍然是按APP1地址链接出来的固件镜像。 */
bool boot_app_backup_is_valid(void)
{
    return boot_app_image_is_valid(BOOT_APP2_START_ADDR);
}

/* 跳转前关闭BOOT已经打开的中断，防止进入APP1后仍触发BOOT中断。 */
static void boot_app_deinit_before_jump(void)
{
    SysTick->CTRL = 0UL;
    SysTick->LOAD = 0UL;
    SysTick->VAL  = 0UL;
    TIM_ConfigInt(TIM6, TIM_INT_UPDATE, DISABLE);
    TIM_Enable(TIM6, DISABLE);
    USART_ConfigInt(USART2, USART_INT_RXDNE, DISABLE);
    USART_Enable(USART2, DISABLE);

    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
}

bool boot_app_jump_app1(void)
{
    uint32_t stack_addr;
    uint32_t reset_addr;

    g_boot_app_jump_stage = 0x10UL;
    g_boot_app_jump_base = BOOT_APP1_START_ADDR;
    if (!boot_app_app1_is_valid())
    {
        g_boot_app_jump_stage = 0xE1UL;
        return false;
    }

    stack_addr = *((volatile const uint32_t *)BOOT_APP1_START_ADDR);
    reset_addr = *((volatile const uint32_t *)(BOOT_APP1_START_ADDR + 4UL));
    g_boot_app_jump_stack = stack_addr;
    g_boot_app_jump_reset = reset_addr;
    g_boot_app_jump_stage = 0x20UL;

    __disable_irq();
    boot_app_deinit_before_jump();
    g_boot_app_jump_vtor = SCB->VTOR;
    g_boot_app_jump_stage = 0x30UL;
    __DSB();
    __ISB();
    __ASM volatile(
        "movs r2, #0\n"
        "msr psp, r2\n"
        "msr control, r2\n"
        "isb\n"
        "msr msp, %0\n"
        "bx %1\n"
        :
        : "r"(stack_addr), "r"(reset_addr)
        : "r2", "memory");

    while (1)
    {
    }

    return true;
}

