/*
 * BOOT侧CAN底层驱动。
 * 中断只负责读取硬件FIFO、写入软件环形队列和记录错误状态；BOOT主循环中的
 * boot_can_task负责发送队列、总线状态轮询以及把队列帧交给升级状态机处理。
 */
#include "Hal_Driver/boot_can.h"

#include "Boot/boot_can_upgrade.h"
#include "UDS/UDSLogic/boot_uds.h"
#include "misc.h"
#include "n32g45x.h"
#include "n32g45x_can.h"
#include "n32g45x_gpio.h"
#include "n32g45x_rcc.h"
#include <string.h>

#define BOOT_CAN_QUEUE_SIZE 256U /* 255 usable slots; one slot separates full/empty. */
#define BOOT_CAN_TIME_QUANTA ((uint32_t)18U) /* 1 sync + 14 TBS1 + 3 TBS2. */
#define BOOT_CAN_BAUDRATE_KBPS ((uint32_t)500U)

typedef struct
{
    uint32_t id;
    uint8_t is_extended;
    uint8_t rtr;
    uint8_t dlc;
    uint8_t data[DUCAR_CAN_MAX_DATA_LEN];
} boot_can_queue_frame_t;

static volatile boot_can_queue_frame_t s_boot_can_rx_queue[BOOT_CAN_QUEUE_SIZE];
static volatile boot_can_queue_frame_t s_boot_can_tx_queue[BOOT_CAN_QUEUE_SIZE];
static volatile uint8_t s_boot_can_rx_head;
static volatile uint8_t s_boot_can_rx_tail;
static volatile uint8_t s_boot_can_tx_head;
static volatile uint8_t s_boot_can_tx_tail;
static volatile uint32_t s_boot_can_rx_overflow_count;
static volatile uint32_t s_boot_can_hw_fifo_overrun_count;
static volatile uint32_t s_boot_can_tx_queue_overflow_count;
static volatile uint32_t s_boot_can_tx_failed_count;
static volatile uint32_t s_boot_can_error_warning_count;
static volatile uint32_t s_boot_can_error_passive_count;
static volatile uint32_t s_boot_can_bus_off_count;
static volatile uint32_t s_boot_can_bus_off_recovery_count;
static volatile uint8_t s_boot_can_last_error_code;
static volatile uint8_t s_boot_can_error_warning;
static volatile uint8_t s_boot_can_error_passive;
static volatile uint8_t s_boot_can_bus_off;
static volatile uint8_t s_boot_can_mailbox_busy[3];
static volatile uint8_t s_boot_can_ready;

/* BOOT和APP必须从同一个G45x PCLK1计算CAN分频，避免两个镜像波特率不一致。 */
/* 根据当前PCLK1和CAN时序参数计算500kbps分频值。 */
static uint16_t boot_can_get_baudrate_prescaler(void)
{
    RCC_ClocksType clocks;
    uint32_t bit_rate_divisor;
    uint32_t baudrate_prescaler;

    RCC_GetClocksFreqValue(&clocks);
    bit_rate_divisor = BOOT_CAN_BAUDRATE_KBPS * 1000UL * BOOT_CAN_TIME_QUANTA;
    if (bit_rate_divisor == 0UL)
    {
        return 0U;
    }

    baudrate_prescaler = (clocks.Pclk1Freq + (bit_rate_divisor / 2UL)) /
                         bit_rate_divisor;
    if ((baudrate_prescaler == 0UL) || (baudrate_prescaler > 1024UL))
    {
        return 0U;
    }

    return (uint16_t)baudrate_prescaler;
}

/* 读取CAN错误状态并统计错误主动、错误被动、Bus-Off及恢复事件。 */
static void boot_can_refresh_error_state(void)
{
    uint8_t warning;
    uint8_t passive;
    uint8_t bus_off;

    warning = (CAN_GetFlagSTS(CAN1, CAN_FLAG_EWGFL) != RESET) ? 1U : 0U;
    passive = (CAN_GetFlagSTS(CAN1, CAN_FLAG_EPVFL) != RESET) ? 1U : 0U;
    bus_off = (CAN_GetFlagSTS(CAN1, CAN_FLAG_BOFFL) != RESET) ? 1U : 0U;
    s_boot_can_last_error_code = CAN_GetLastErrCode(CAN1);

    if ((warning != 0U) && (s_boot_can_error_warning == 0U))
    {
        s_boot_can_error_warning_count++;
    }
    if ((passive != 0U) && (s_boot_can_error_passive == 0U))
    {
        s_boot_can_error_passive_count++;
    }
    if ((bus_off != 0U) && (s_boot_can_bus_off == 0U))
    {
        s_boot_can_bus_off_count++;
    }
    if ((bus_off == 0U) && (s_boot_can_bus_off != 0U))
    {
        /* ABOM clears BOFFL after the required recessive bus recovery. */
        s_boot_can_bus_off_recovery_count++;
    }

    s_boot_can_error_warning = warning;
    s_boot_can_error_passive = passive;
    s_boot_can_bus_off = bus_off;
}

/* 配置BOOT使用的CAN1引脚、CAN唤醒输入和隔离侧5V使能。 */
static void boot_can_config_gpio(void)
{
    GPIO_InitType gpio_init;

    /* 当前原理图使用CAN1默认引脚：PA11=RX、PA12=TX。 */
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO |
                             RCC_APB2_PERIPH_GPIOA |
                             RCC_APB2_PERIPH_GPIOC, ENABLE);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_11;
    gpio_init.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitPeripheral(GPIOA, &gpio_init);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_12;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &gpio_init);

    /* CAN_WK is an active-low optocoupler output with an external pull-up. */
    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_8;
    gpio_init.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitPeripheral(GPIOC, &gpio_init);

    /* PA11/PA12 are CAN1 default pins; make sure both remap stages are off. */
    GPIO_ConfigPinRemap(GPIO_RMP1_CAN1, DISABLE);
    GPIO_ConfigPinRemap(GPIO_RMP2_CAN1, DISABLE);

    /* IOS5V_EN keeps the isolated CAN side powered while BOOT is active. */
    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_9;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitPeripheral(GPIOC, &gpio_init);
    GPIO_WriteBit(GPIOC, GPIO_PIN_9, Bit_SET);
}

/* 初始化CAN1工作模式、过滤器、中断和ABOM总线关闭自动恢复。 */
static bool boot_can_config_peripheral(void)
{
    CAN_InitType can_init;
    CAN_FilterInitType filter_init;
    NVIC_InitType nvic_init;
    uint16_t baudrate_prescaler;

    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_CAN1, ENABLE);
    baudrate_prescaler = boot_can_get_baudrate_prescaler();
    if (baudrate_prescaler == 0U)
    {
        return false;
    }

    CAN_DeInit(CAN1);
    CAN_InitStruct(&can_init);
    can_init.TTCM = DISABLE;
    can_init.ABOM = ENABLE;
    can_init.AWKUM = DISABLE;
    can_init.NART = DISABLE;
    can_init.RFLM = DISABLE;
    can_init.TXFP = ENABLE;
    can_init.OperatingMode = CAN_Normal_Mode;
    can_init.RSJW = CAN_RSJW_1tq;
    can_init.TBS1 = CAN_TBS1_14tq;
    can_init.TBS2 = CAN_TBS2_3tq;
    can_init.BaudRatePrescaler = baudrate_prescaler;

    if (CAN_Init(CAN1, &can_init) != CAN_InitSTS_Success)
    {
        return false;
    }

    /* Accept all standard data frames for future standard-frame services. */
    filter_init.Filter_Num = 0U;
    filter_init.Filter_Mode = CAN_Filter_IdMaskMode;
    filter_init.Filter_Scale = CAN_Filter_32bitScale;
    filter_init.Filter_HighId = 0U;
    filter_init.Filter_LowId = 0U;
    filter_init.FilterMask_HighId = 0U;
    filter_init.FilterMask_LowId = 0x0006U;
    filter_init.Filter_FIFOAssignment = CAN_FIFO0;
    filter_init.Filter_Act = ENABLE;
    CAN1_InitFilter(&filter_init);

    /* Accept all extended data frames used by DUCAR CAN upgrade. */
    filter_init.Filter_Num = 1U;
    filter_init.Filter_LowId = 0x0004U;
    CAN1_InitFilter(&filter_init);
    CAN_INTConfig(CAN1, CAN_INT_FMP0, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_FOV0, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_TME, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_EWG, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_EPV, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_BOF, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_LEC, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_ERR, ENABLE);

    nvic_init.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0U;
    nvic_init.NVIC_IRQChannelSubPriority = 0U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);

    nvic_init.NVIC_IRQChannel = USB_HP_CAN1_TX_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0U;
    nvic_init.NVIC_IRQChannelSubPriority = 1U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);

    nvic_init.NVIC_IRQChannel = CAN1_SCE_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 1U;
    nvic_init.NVIC_IRQChannelSubPriority = 0U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);
    return true;
}

/* 尝试将一帧放入CAN硬件邮箱；邮箱满或Bus-Off时返回失败。 */
static bool boot_can_try_transmit(const boot_can_queue_frame_t *frame)
{
    CanTxMessage message;

    if ((frame == 0) || (frame->dlc > DUCAR_CAN_MAX_DATA_LEN) ||
        (s_boot_can_bus_off != 0U))
    {
        return false;
    }

    memset(&message, 0, sizeof(message));
    message.IDE = (frame->is_extended != 0U) ? CAN_Extended_Id : CAN_Standard_Id;
    message.RTR = CAN_RTRQ_DATA;
    message.DLC = frame->dlc;
    if (frame->is_extended != 0U)
    {
        message.ExtId = frame->id & DUCAR_CAN_EXT_ID_MASK;
    }
    else
    {
        message.StdId = frame->id & 0x7FFUL;
    }
    memcpy(message.Data, frame->data, frame->dlc);
    {
        uint8_t mailbox = CAN_TransmitMessage(CAN1, &message);
        if (mailbox == CAN_STS_NO_MB)
        {
            return false;
        }
        s_boot_can_mailbox_busy[mailbox] = 1U;
        return true;
    }
}

/* 清除指定发送邮箱的完成/失败状态位。 */
static void boot_can_clear_mailbox_status(uint8_t mailbox)
{
    if (mailbox == 0U)
    {
        CAN_ClearFlag(CAN1, CAN_FLAG_RQCPM0);
    }
    else if (mailbox == 1U)
    {
        CAN_ClearFlag(CAN1, CAN_FLAG_RQCPM1);
    }
    else
    {
        CAN_ClearFlag(CAN1, CAN_FLAG_RQCPM2);
    }
}

/* 回收已完成或失败的硬件邮箱，并更新发送失败统计。 */
static void boot_can_service_tx_status(void)
{
    uint8_t mailbox;
    uint8_t status;

    for (mailbox = 0U; mailbox < 3U; mailbox++)
    {
        if (s_boot_can_mailbox_busy[mailbox] == 0U)
        {
            continue;
        }
        status = CAN_TransmitSTS(CAN1, mailbox);
        if ((status == CAN_TxSTS_Ok) || (status == CAN_TxSTS_Failed))
        {
            s_boot_can_mailbox_busy[mailbox] = 0U;
            if (status == CAN_TxSTS_Failed)
            {
                s_boot_can_tx_failed_count++;
            }
            boot_can_clear_mailbox_status(mailbox);
        }
    }
}

/* 将硬件暂时不能发送的帧写入BOOT发送环形队列。 */
static bool boot_can_enqueue_tx(const boot_can_queue_frame_t *frame)
{
    uint8_t next;

    next = (uint8_t)((s_boot_can_tx_tail + 1U) % BOOT_CAN_QUEUE_SIZE);
    if (next == s_boot_can_tx_head)
    {
        s_boot_can_tx_queue_overflow_count++;
        return false;
    }

    memcpy((void *)&s_boot_can_tx_queue[s_boot_can_tx_tail], frame, sizeof(*frame));
    s_boot_can_tx_tail = next;
    return true;
}

/* 按先进先出顺序把软件发送队列尽可能提交到硬件邮箱。 */
static void boot_can_flush_tx(void)
{
    boot_can_queue_frame_t frame;

    while (s_boot_can_tx_head != s_boot_can_tx_tail)
    {
        memcpy(&frame, (const void *)&s_boot_can_tx_queue[s_boot_can_tx_head], sizeof(frame));
        if (!boot_can_try_transmit(&frame))
        {
            break;
        }
        s_boot_can_tx_head = (uint8_t)((s_boot_can_tx_head + 1U) % BOOT_CAN_QUEUE_SIZE);
    }
}

/* BOOT协议层调用的统一发送接口，先直发，失败后进入发送环形队列。 */
bool boot_can_send_frame(const ducar_can_frame_t *frame)
{
    boot_can_queue_frame_t tx_frame;
    ducar_can_id_fields_t fields;

    if ((s_boot_can_ready == 0U) || (frame == 0) ||
        (frame->dlc > DUCAR_CAN_MAX_DATA_LEN))
    {
        return false;
    }

    if (frame->is_extended != 0U)
    {
        if (!ducar_can_validate_id(frame->id, &fields))
        {
            return false;
        }
    }
    else if (frame->id > 0x7FFUL)
    {
        return false;
    }

    tx_frame.id = frame->id;
    tx_frame.is_extended = frame->is_extended;
    tx_frame.rtr = CAN_RTRQ_DATA;
    tx_frame.dlc = frame->dlc;
    memcpy(tx_frame.data, frame->data, frame->dlc);

    if (s_boot_can_tx_head != s_boot_can_tx_tail)
    {
        return boot_can_enqueue_tx(&tx_frame);
    }

    if (boot_can_try_transmit(&tx_frame))
    {
        return true;
    }

    return boot_can_enqueue_tx(&tx_frame);
}

/* 从接收环形队列取出一帧，供BOOT主循环分发给升级协议。 */
static bool boot_can_pop_rx(boot_can_queue_frame_t *frame)
{
    if ((frame == 0) || (s_boot_can_rx_head == s_boot_can_rx_tail))
    {
        return false;
    }

    memcpy(frame, (const void *)&s_boot_can_rx_queue[s_boot_can_rx_head], sizeof(*frame));
    s_boot_can_rx_head = (uint8_t)((s_boot_can_rx_head + 1U) % BOOT_CAN_QUEUE_SIZE);
    return true;
}

/* 清空队列和错误统计，配置GPIO/CAN，并启动BOOT CAN升级模块。 */
void boot_can_init(void)
{
    s_boot_can_rx_head = 0U;
    s_boot_can_rx_tail = 0U;
    s_boot_can_tx_head = 0U;
    s_boot_can_tx_tail = 0U;
    s_boot_can_rx_overflow_count = 0UL;
    s_boot_can_hw_fifo_overrun_count = 0UL;
    s_boot_can_tx_queue_overflow_count = 0UL;
    s_boot_can_tx_failed_count = 0UL;
    s_boot_can_error_warning_count = 0UL;
    s_boot_can_error_passive_count = 0UL;
    s_boot_can_bus_off_count = 0UL;
    s_boot_can_bus_off_recovery_count = 0UL;
    s_boot_can_last_error_code = 0U;
    s_boot_can_error_warning = 0U;
    s_boot_can_error_passive = 0U;
    s_boot_can_bus_off = 0U;
    memset((void *)s_boot_can_mailbox_busy, 0, sizeof(s_boot_can_mailbox_busy));
    s_boot_can_ready = 0U;
    boot_can_config_gpio();
    s_boot_can_ready = boot_can_config_peripheral() ? 1U : 0U;
    boot_can_upgrade_init(boot_can_send_frame);
}

/* 关闭CAN相关中断和外设；用于异常恢复时重新建立干净的硬件状态。 */
void boot_can_deinit(void)
{
    if (s_boot_can_ready != 0U)
    {
        CAN_INTConfig(CAN1, CAN_INT_FMP0, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_FOV0, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_TME, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_EWG, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_EPV, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_BOF, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_LEC, DISABLE);
        CAN_INTConfig(CAN1, CAN_INT_ERR, DISABLE);
        NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
        NVIC_DisableIRQ(USB_HP_CAN1_TX_IRQn);
        NVIC_DisableIRQ(CAN1_SCE_IRQn);
        CAN_DeInit(CAN1);
        s_boot_can_ready = 0U;
    }
}

/* BOOT CAN主循环：轮询错误、维护发送、执行升级任务并排空接收队列。 */
void boot_can_task(void)
{
    boot_can_queue_frame_t rx_frame;
    ducar_can_frame_t frame;
    uint32_t primask;

    if (s_boot_can_ready == 0U)
    {
        return;
    }

    /* Error IRQs are edge-triggered; poll also observes ABOM recovery. */
    primask = __get_PRIMASK();
    __disable_irq();
    boot_can_refresh_error_state();
    __set_PRIMASK(primask);
    boot_can_service_tx_status();
    boot_can_flush_tx();
    boot_can_upgrade_task();
    while (boot_can_pop_rx(&rx_frame))
    {
        if (rx_frame.is_extended != 0U)
        {
            ducar_can_frame_init(&frame, rx_frame.id);
            frame.is_extended = 1U;
            frame.dlc = rx_frame.dlc;
            memcpy(frame.data, rx_frame.data, sizeof(frame.data));
            (void)boot_can_upgrade_on_receive(&frame);
        }
        else if ((rx_frame.id == 0x7A0UL) || (rx_frame.id == 0x7DFUL))
        {
            boot_uds_receive_frame(rx_frame.id,
                                   (const uint8_t *)rx_frame.data,
                                   rx_frame.dlc);
        }
    }
    boot_can_service_tx_status();
    boot_can_flush_tx();
}

/* 判断软件发送队列和三个硬件邮箱是否都已空闲。 */
bool boot_can_is_tx_idle(void)
{
    uint8_t mailbox;

    boot_can_service_tx_status();
    if (s_boot_can_tx_head != s_boot_can_tx_tail)
    {
        return false;
    }
    for (mailbox = 0U; mailbox < 3U; mailbox++)
    {
        if (s_boot_can_mailbox_busy[mailbox] != 0U)
        {
            return false;
        }
    }
    return true;
}

/* 获取硬件发送失败累计次数，供升级完成等待阶段判断异常。 */
uint32_t boot_can_get_tx_failed_count(void)
{
    return s_boot_can_tx_failed_count;
}

/* 获取软件接收环形队列溢出次数。 */
uint32_t boot_can_get_rx_overflow_count(void)
{
    return s_boot_can_rx_overflow_count;
}

/* 获取CAN硬件FIFO溢出次数。 */
uint32_t boot_can_get_hw_fifo_overrun_count(void)
{
    return s_boot_can_hw_fifo_overrun_count;
}

/* 获取软件发送环形队列溢出次数。 */
uint32_t boot_can_get_tx_queue_overflow_count(void)
{
    return s_boot_can_tx_queue_overflow_count;
}

/* 获取进入错误主动阈值以上的事件次数。 */
uint32_t boot_can_get_error_warning_count(void)
{
    return s_boot_can_error_warning_count;
}

/* 获取进入错误被动状态的事件次数。 */
uint32_t boot_can_get_error_passive_count(void)
{
    return s_boot_can_error_passive_count;
}

/* 获取进入Bus-Off状态的事件次数。 */
uint32_t boot_can_get_bus_off_count(void)
{
    return s_boot_can_bus_off_count;
}

/* 获取硬件ABOM完成总线关闭恢复的事件次数。 */
uint32_t boot_can_get_bus_off_recovery_count(void)
{
    return s_boot_can_bus_off_recovery_count;
}

/* 获取CAN控制器最近一次LEC错误码。 */
uint8_t boot_can_get_last_error_code(void)
{
    return s_boot_can_last_error_code;
}

/* CAN错误中断入口：清除已确认的中断源，保留BOFFL等待硬件恢复序列。 */
void boot_can_error_irq_handler(void)
{
    FlagStatus bus_off_flag;

    if (s_boot_can_ready == 0U)
    {
        boot_can_deinit();
        boot_can_init();

        bus_off_flag = CAN_GetFlagSTS(CAN1, CAN_FLAG_BOFFL);
        if (bus_off_flag != RESET)
        {
            /* Clear only the SCE request; keep BOFFL for hardware recovery. */
            CAN_ClearINTPendingBit(CAN1, CAN_INT_BOF);
            return;
        }

        /* Clear the last-error request only when the LEC source is active. */
        if (CAN_GetIntStatus(CAN1, CAN_INT_LEC) != RESET)
        {
            CAN_ClearINTPendingBit(CAN1, CAN_INT_LEC);
        }

        /* Clear the common CAN error request only when it is still asserted. */
        if (CAN_GetIntStatus(CAN1, CAN_INT_ERR) != RESET)
        {
            CAN_ClearINTPendingBit(CAN1, CAN_INT_ERR);
        }
        return;
    }

    boot_can_refresh_error_state();

    /*
     * BOFFL is a hardware state flag. It must not be cleared by software or
     * by switching CAN to initialization mode. ABOM is enabled during CAN
     * initialization, so the CAN controller will leave bus-off only after
     * it has detected the CAN-standard recovery sequence on CANRX.
     */
    bus_off_flag = CAN_GetFlagSTS(CAN1, CAN_FLAG_BOFFL);
    if (bus_off_flag != RESET)
    {
        /* Clear only the SCE request; keep BOFFL for hardware recovery. */
        CAN_ClearINTPendingBit(CAN1, CAN_INT_BOF);
        return;
    }

    /* Clear the last-error request only when the LEC source is active. */
    if (CAN_GetIntStatus(CAN1, CAN_INT_LEC) != RESET)
    {
        CAN_ClearINTPendingBit(CAN1, CAN_INT_LEC);
    }

    /* Clear the common CAN error request only when it is still asserted. */
    if (CAN_GetIntStatus(CAN1, CAN_INT_ERR) != RESET)
    {
        CAN_ClearINTPendingBit(CAN1, CAN_INT_ERR);
    }
}

/* CAN接收中断入口：清FIFO并把有效数据帧放入接收环形队列。 */
void boot_can_irq_handler(void)
{
    CanRxMessage message;
    uint8_t next;

    if (CAN_GetIntStatus(CAN1, CAN_INT_FOV0) != RESET)
    {
        s_boot_can_hw_fifo_overrun_count++;
        CAN_ClearINTPendingBit(CAN1, CAN_INT_FOV0);
    }

    while (CAN_PendingMessage(CAN1, CAN_FIFO0) != 0U)
    {
        CAN_ReceiveMessage(CAN1, CAN_FIFO0, &message);

        if (((message.IDE != CAN_Extended_Id) &&
             (message.IDE != CAN_Standard_Id)) ||
            (message.RTR != CAN_RTRQ_DATA) ||
            (message.DLC > DUCAR_CAN_MAX_DATA_LEN))
        {
            continue;
        }

        next = (uint8_t)((s_boot_can_rx_tail + 1U) % BOOT_CAN_QUEUE_SIZE);
        if (next == s_boot_can_rx_head)
        {
            s_boot_can_rx_overflow_count++;
            continue;
        }
        s_boot_can_rx_queue[s_boot_can_rx_tail].is_extended =
            (message.IDE == CAN_Extended_Id) ? 1U : 0U;
        s_boot_can_rx_queue[s_boot_can_rx_tail].rtr = message.RTR;
        s_boot_can_rx_queue[s_boot_can_rx_tail].id =
            (s_boot_can_rx_queue[s_boot_can_rx_tail].is_extended != 0U) ?
            (message.ExtId & DUCAR_CAN_EXT_ID_MASK) :
            (message.StdId & 0x7FFUL);
        s_boot_can_rx_queue[s_boot_can_rx_tail].dlc =
            (message.DLC <= DUCAR_CAN_MAX_DATA_LEN) ? message.DLC : DUCAR_CAN_MAX_DATA_LEN;
        memcpy((void *)s_boot_can_rx_queue[s_boot_can_rx_tail].data,
               message.Data, DUCAR_CAN_MAX_DATA_LEN);
        s_boot_can_rx_tail = next;
    }
}

/* CAN发送邮箱中断入口：回收硬件发送状态，实际队列发送由主循环完成。 */
void USB_HP_CAN1_TX_IRQHandler(void)
{
    boot_can_service_tx_status();
}
