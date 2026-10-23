#ifndef __BOOT_TIMER_H__
#define __BOOT_TIMER_H__

#include <stdint.h>

void boot_timer_init(void);
uint32_t boot_timer_get_ms(void);
void boot_timer_delay_ms(uint32_t ms);
void boot_timer_irq_handler(void);

#endif
