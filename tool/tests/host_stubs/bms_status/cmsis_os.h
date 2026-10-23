#ifndef __CMSIS_OS_H__
#define __CMSIS_OS_H__

#include <stdint.h>

#define osKernelSysTickFrequency (1000U)
uint32_t osKernelSysTick(void);

#endif
