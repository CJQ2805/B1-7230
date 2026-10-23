#ifndef STACK_MONITOR_TASK_H
#define STACK_MONITOR_TASK_H

#include "cmsis_os.h"

#define APP_STACK_MONITOR_PERIOD_MS  (1000U)

void StackMonitor_Task(void const *argument);

#endif /* STACK_MONITOR_TASK_H */
