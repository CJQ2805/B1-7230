# N32G45x FreeRTOS 系统层移植设计

## 目标

第一阶段仅把 FreeRTOS 系统层接入当前 `N32G452REL7` APP 工程，并让现有协作式业务循环作为唯一 `defaultTask` 运行；不在本阶段拆分或改写 BMS、AFE、通信、LOG 业务。

## 已确认约束

- 参考工程使用 FreeRTOS + CMSIS-RTOS v1，并通过 `osThreadDef`、`osThreadCreate` 和 `osDelay` 创建任务。
- `N32G452REL7` 是 Cortex-M4F，不能使用参考工程的 `ARM_CM0` 端口。
- N32G45 官方库提供可用的 `GCC/ARM_CM4F` FreeRTOS 端口；内核配置使用 1 kHz tick、4 个中断优先级位和动态 `heap_4`。
- 当前工程已经通过 Keil 编译，APP 链接地址为 `0x0800C000`，本阶段必须保持 Flash 布局和业务初始化顺序不变。
- 后续业务阶段按 5 个业务任务 + 1 个默认任务导入：`BMS_Task`、`AFE_Task`、`Comm_Task`、`DataHandle_Task`、`SaveLog_Task`、`defaultTask`。

## 系统层设计

### FreeRTOS 来源

- FreeRTOS 核心文件、CMSIS-RTOS v1 适配层沿用用户指定的参考工程接口和组织方式。
- Cortex-M4F 的 `port.c`、`portmacro.h` 采用 N32G45 官方库的 `GCC/ARM_CM4F` 实现；当前 Keil 工程使用 ARMCLANG V6，不能直接编译参考工程的 ARM Compiler 5 RVDS 语法。
- 仅加入核心、队列、事件组、软件定时器、流缓冲区、内存管理和 CMSIS-RTOS 适配文件；不把参考工程的 STM32 HAL 或业务目录带入当前工程。

### 启动方式

1. `main()` 保持现有硬件初始化和业务初始化顺序。
2. 初始化完成后创建一个 `defaultTask`，任务入口暂时调用当前主循环。
3. `osKernelStart()` 启动调度器；启动后 `main()` 不再执行业务循环。
4. `defaultTask` 每轮执行原有协作式调度，并在末尾 `osDelay(1)`，为后续 5 个业务任务导入保留调度空间。

### Cortex-M 异常入口

- `SVC_Handler`、`PendSV_Handler`、`SysTick_Handler` 由 FreeRTOS `FreeRTOSConfig.h` 映射到端口实现。
- 删除当前空的 `SVC_Handler` 和 `SysTick_Handler`，避免与 FreeRTOS 端口重复定义。
- 保留 `HardFault_Handler` 和现有外设中断入口。
- 本阶段不在 TIM6、TIM7、UART 中断里调用 FreeRTOS API；后续业务任务导入时再按 `configMAX_SYSCALL_INTERRUPT_PRIORITY` 审核中断优先级。

## 配置

- `configCPU_CLOCK_HZ = SystemCoreClock`
- `configTICK_RATE_HZ = 1000`
- `configMAX_PRIORITIES = 7`
- `configTOTAL_HEAP_SIZE = 20 KiB`
- `configUSE_PREEMPTION = 1`
- `configUSE_MUTEXES = 1`
- `configUSE_COUNTING_SEMAPHORES = 1`
- `configCHECK_FOR_STACK_OVERFLOW = 2`
- `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5`
- `vPortSVCHandler = SVC_Handler`
- `xPortPendSVHandler = PendSV_Handler`
- `xPortSysTickHandler = SysTick_Handler`

## 验证标准

1. FreeRTOS 核心和 M4F 端口源文件全部加入 `app/MDK-ARM/N32G45x.uvprojx`。
2. APP 工程仍使用 `N32G452REL7`、`0x0800C000` 和原有 Flash scatter 配置。
3. Keil 重建无错误、无警告。
4. 生成的 map 中包含 `vTaskStartScheduler` 以及映射后的 `SVC_Handler`、`PendSV_Handler`、`SysTick_Handler` 入口，且没有空的重复系统异常入口。
5. 当前业务函数仍保持原有调用顺序；5 个业务任务的拆分不在本阶段出现。

## 非目标

- 不在本阶段创建 BMS、AFE、通信、DataHandle、SaveLog 五个业务任务。
- 不修改业务协议、故障阈值、Flash 地址、低功耗策略或驱动 API。
- 不把 BOOT 工程接入 FreeRTOS；BOOT 继续保持独立、确定性的升级启动流程。
