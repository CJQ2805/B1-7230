#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* N32G452VEL7 / Cortex-M4F FreeRTOS configuration. */
#include <stdint.h>
#include "n32g45x.h"

extern uint32_t SystemCoreClock;

#define configENABLE_FPU                         1
#define configENABLE_MPU                         0

#define configUSE_PREEMPTION                     1
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configCPU_CLOCK_HZ                       (SystemCoreClock)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     (7)
/* FreeRTOS 原生栈深度单位是 StackType_t 字，256 字对应 1 KiB Idle 栈。 */
#define configMINIMAL_STACK_SIZE                 ((uint16_t)256)
#define configTOTAL_HEAP_SIZE                    ((size_t)(32U * 1024U))
#define configMAX_TASK_NAME_LEN                  (16)
#define configUSE_TRACE_FACILITY                 0
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configUSE_MUTEXES                        1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES             1
#define configCHECK_FOR_STACK_OVERFLOW           2
#define configUSE_MALLOC_FAILED_HOOK             1
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0

/* Co-routine definitions. */
#define configUSE_CO_ROUTINES                    0
#define configMAX_CO_ROUTINE_PRIORITIES          (2)

/* Software timer service is not needed until the business-task phase. */
#define configUSE_TIMERS                         0

/* FreeRTOS API inclusion. */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskCleanUpResources            0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xQueueGetMutexHolder            1
#define INCLUDE_xSemaphoreGetMutexHolder        1

/* Cortex-M interrupt priority configuration. */
#define configPRIO_BITS                         __NVIC_PRIO_BITS
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY         (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* Keep the system stopped if a kernel invariant is violated. */
#define configASSERT(x)                          \
    if ((x) == 0)                                \
    {                                            \
        taskDISABLE_INTERRUPTS();               \
        for (;;)                                 \
        {                                        \
        }                                        \
    }

/* Map the Cortex-M exception names to the FreeRTOS port implementation. */
#define vPortSVCHandler                         SVC_Handler
#define xPortPendSVHandler                      PendSV_Handler
/* SysTick_Handler is a small wrapper in d_timer.c, matching the template.
 * It avoids entering the kernel tick handler before the scheduler starts. */

#endif /* FREERTOS_CONFIG_H */
