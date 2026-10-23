/*
 * 文件作用：N32G45x CAN1底层驱动。
 *
 * 本层只处理CAN硬件相关内容：
 *   1. PA11/PA12引脚、CAN时钟、波特率和验收过滤器；
 *   2. RX FIFO中断、系统错误中断和硬件发送邮箱；
 *   3. 原始报文进入/离开接收环形队列。
 *
 * 本层不解析PF、PGN、E1、DF00、UDS等协议字段。协议解析统一由APP层
 * 或BOOT层的协议模块完成，避免底层驱动依赖具体业务。
 */
#include "Driver/d_can.h"

#include <string.h>

#include "App/app_param.h"
#include "Driver/d_timer.h"
#include "misc.h"
#include "n32g45x.h"
#include "n32g45x_can.h"
#include "n32g45x_gpio.h"
#include "n32g45x_rcc.h"

#define D_CAN_STD_ID_MAX       ((uint32_t)0x000007FFUL)
#define D_CAN_EXT_ID_MAX       ((uint32_t)0x1FFFFFFFUL)
#define D_CAN_TIME_QUANTA      ((uint32_t)18U) /* 1 + 14 + 3 TQ. */
#define D_CAN_RX_ISR_MAX_FRAMES ((uint8_t)3U)
#define D_CAN_TX_MAILBOX_COUNT ((uint8_t)3U)
#define D_CAN_TX_MAX_RETRIES   ((uint8_t)1U)
#define D_CAN_TX_OFFLINE_FAIL_LIMIT ((uint8_t)3U)
#define D_CAN_TX_TIMEOUT_MS    ((uint32_t)250U)

struct d_can_t
{
    CAN_Module *handler;
};

d_can_t can0 = {CAN1};

typedef struct
{
    CanTxMessage message;
    uint32_t start_ms;
    uint8_t retry_count;
    uint8_t active;
    uint8_t abort_requested;
    uint8_t is_iap;
} d_can_tx_mailbox_state_t;

static volatile uint8_t s_can_ready;
static volatile bool s_extended_tx_enabled = true;
static volatile bool s_can_iap_silence_active;
static d_can_tx_mailbox_state_t s_can_tx_mailbox_state[D_CAN_TX_MAILBOX_COUNT];
static t_can_packet s_can_rx_packets[D_CAN_RX_QUEUE_DEPTH];
static volatile uint8_t s_can_rx_write_index;
static volatile uint8_t s_can_rx_read_index;
static volatile uint32_t s_can_rx_activity_count;
static volatile uint32_t s_can_tx_activity_count;
static volatile uint32_t s_can_tx_request_count;
static volatile uint32_t s_can_tx_mailbox_accept_count;
static volatile uint32_t s_can_tx_no_mailbox_count;
static volatile uint32_t s_can_tx_invalid_reject_count;
static volatile uint32_t s_can_tx_busoff_reject_count;
static volatile uint32_t s_can_tx_complete_count;
static volatile uint32_t s_can_tx_failed_count;
static volatile uint32_t s_can_tx_retry_count;
static volatile uint32_t s_can_tx_terminal_fail_count;
static volatile uint32_t s_can_tx_timeout_count;
static volatile uint32_t s_can_error_count;
static volatile uint32_t s_can_ack_error_count;
static volatile uint32_t s_can_other_error_count;
static volatile uint32_t s_can_last_attempt_id;
static volatile uint8_t s_can_last_error_code;
static volatile uint8_t s_can_tx_fail_streak;
static volatile uint8_t s_can_bus_online = 1U;

static void d_can_tx_collect_completed(void)
{
    static const uint32_t au32RequestCompleteFlags[D_CAN_TX_MAILBOX_COUNT] =
    {
        CAN_FLAG_RQCPM0,
        CAN_FLAG_RQCPM1,
        CAN_FLAG_RQCPM2
    };
    uint8_t u8Mailbox;

    if (s_can_ready == 0U)
    {
        return;
    }

    for (u8Mailbox = 0U; u8Mailbox < D_CAN_TX_MAILBOX_COUNT; ++u8Mailbox)
    {
        if (CAN_GetFlagSTS(CAN1, au32RequestCompleteFlags[u8Mailbox]) != RESET)
        {
            uint8_t u8TxStatus = CAN_TransmitSTS(CAN1, u8Mailbox);

            CAN_ClearFlag(CAN1, au32RequestCompleteFlags[u8Mailbox]);
            if (u8TxStatus == CAN_TxSTS_Ok)
            {
                s_can_tx_complete_count++;
                s_can_tx_fail_streak = 0U;
                s_can_bus_online = 1U;
                s_can_tx_mailbox_state[u8Mailbox].active = 0U;
            }
            else
            {
                s_can_tx_failed_count++;
                if ((s_can_tx_mailbox_state[u8Mailbox].active != 0U) &&
                    ((s_can_iap_silence_active == false) ||
                     (s_can_tx_mailbox_state[u8Mailbox].is_iap != 0U)) &&
                    (s_can_tx_mailbox_state[u8Mailbox].retry_count < D_CAN_TX_MAX_RETRIES))
                {
                    CanTxMessage tRetryMessage = s_can_tx_mailbox_state[u8Mailbox].message;
                    uint8_t u8RetryCount = (uint8_t)(s_can_tx_mailbox_state[u8Mailbox].retry_count + 1U);
                    uint8_t u8RetryIsIap = s_can_tx_mailbox_state[u8Mailbox].is_iap;
                    uint8_t u8RetryMailbox;

                    s_can_tx_mailbox_state[u8Mailbox].active = 0U;
                    s_can_tx_mailbox_state[u8Mailbox].is_iap = 0U;
                    u8RetryMailbox = CAN_TransmitMessage(CAN1, &tRetryMessage);
                    if (u8RetryMailbox < D_CAN_TX_MAILBOX_COUNT)
                    {
                        s_can_tx_mailbox_state[u8RetryMailbox].message = tRetryMessage;
                        s_can_tx_mailbox_state[u8RetryMailbox].start_ms = d_timer_get_ms();
                        s_can_tx_mailbox_state[u8RetryMailbox].retry_count = u8RetryCount;
                        s_can_tx_mailbox_state[u8RetryMailbox].active = 1U;
                        s_can_tx_mailbox_state[u8RetryMailbox].abort_requested = 0U;
                        s_can_tx_mailbox_state[u8RetryMailbox].is_iap = u8RetryIsIap;
                        s_can_tx_retry_count++;
                        /* 软件重试也是实际CAN发送活动，应参与休眠静默计时。 */
                        s_can_tx_activity_count++;
                        continue;
                    }
                }

                s_can_tx_mailbox_state[u8Mailbox].active = 0U;
                s_can_tx_mailbox_state[u8Mailbox].is_iap = 0U;
                s_can_tx_terminal_fail_count++;
                if (s_can_tx_fail_streak < 0xFFU)
                {
                    s_can_tx_fail_streak++;
                }
                if (s_can_tx_fail_streak >= D_CAN_TX_OFFLINE_FAIL_LIMIT)
                {
                    s_can_bus_online = 0U;
                }
            }
        }
    }
}

void d_can_service_tx(void)
{
    uint32_t u32NowMs;
    uint32_t primask;
    uint8_t u8Mailbox;

    if (s_can_ready == 0U)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    if (s_can_ready == 0U)
    {
        __set_PRIMASK(primask);
        return;
    }

    d_can_tx_collect_completed();
    u32NowMs = d_timer_get_ms();
    for (u8Mailbox = 0U; u8Mailbox < D_CAN_TX_MAILBOX_COUNT; ++u8Mailbox)
    {
        if ((s_can_tx_mailbox_state[u8Mailbox].active != 0U) &&
            (s_can_tx_mailbox_state[u8Mailbox].abort_requested == 0U) &&
            ((uint32_t)(u32NowMs - s_can_tx_mailbox_state[u8Mailbox].start_ms) >= D_CAN_TX_TIMEOUT_MS))
        {
            if (CAN_TransmitSTS(CAN1, u8Mailbox) == CAN_TxSTS_Pending)
            {
                /* 对端长期未ACK时释放硬件邮箱，完成标志由后续collector统一处理。 */
                CAN_CancelTransmitMessage(CAN1, u8Mailbox);
                s_can_tx_mailbox_state[u8Mailbox].abort_requested = 1U;
                s_can_tx_timeout_count++;
            }
            else
            {
                d_can_tx_collect_completed();
            }
        }
    }
    __set_PRIMASK(primask);
}

/* 只允许当前工程的CAN1实例进入底层操作，防止传入错误句柄。 */
static bool d_can_is_instance_valid(const d_can_t *can)
{
    return ((can == &can0) && (can->handler == CAN1));
}

void d_can_set_iap_silence(bool enabled)
{
    uint32_t primask;
    uint8_t u8Mailbox;

    primask = __get_PRIMASK();
    __disable_irq();
    if (!enabled)
    {
        s_can_iap_silence_active = false;
        __set_PRIMASK(primask);
        return;
    }

    if (s_can_iap_silence_active == false)
    {
        /* 先置静默标记，完成状态收集时就不会重试普通失败帧。 */
        s_can_iap_silence_active = true;
        if (s_can_ready != 0U)
        {
            d_can_tx_collect_completed();

            /* 升级开始前已经提交的普通报文取消，避免静默后仍续发或重试。 */
            for (u8Mailbox = 0U; u8Mailbox < D_CAN_TX_MAILBOX_COUNT; ++u8Mailbox)
            {
                if ((s_can_tx_mailbox_state[u8Mailbox].active != 0U) &&
                    (s_can_tx_mailbox_state[u8Mailbox].is_iap == 0U) &&
                    (CAN_TransmitSTS(CAN1, u8Mailbox) == CAN_TxSTS_Pending))
                {
                    CAN_CancelTransmitMessage(CAN1, u8Mailbox);
                    s_can_tx_mailbox_state[u8Mailbox].abort_requested = 1U;
                }
            }
        }
    }
    __set_PRIMASK(primask);
}

/*
 * 将CAN硬件报文转换成t_can_packet并写入接收环形队列。
 * 该函数运行在RX中断上下文，必须保持短小：只做校验、复制和入队，
 * 不调用协议函数，不做耗时Flash操作。
 */
static void d_can_rx_data_handle(const CanRxMessage *pCanRxMessage)
{
    t_can_packet tCanPacket;
    uint8_t u8WriteIndex;
    uint8_t u8NextWriteIndex;

    if (pCanRxMessage == 0)
    {
        return;
    }

    /* 即使协议不处理或接收队列已满，线路上仍发生了CAN通信。 */
    s_can_rx_activity_count++;
    /* 任意收到的CAN帧都证明总线上仍有节点活动，可恢复周期发送。 */
    s_can_tx_fail_streak = 0U;
    s_can_bus_online = 1U;
    if ((pCanRxMessage->RTR != CAN_RTRQ_Data) ||
        (pCanRxMessage->DLC > D_CAN_MAX_DATA_LEN))
    {
        return;
    }

    memset(&tCanPacket, 0, sizeof(tCanPacket));
    if (pCanRxMessage->IDE == CAN_Standard_Id)
    {
        tCanPacket.u32id = pCanRxMessage->StdId;
        tCanPacket.u32idType = D_CAN_ID_STANDARD;
    }
    else if (pCanRxMessage->IDE == CAN_Extended_Id)
    {
        tCanPacket.u32id = pCanRxMessage->ExtId;
        tCanPacket.u32idType = D_CAN_ID_EXTENDED;
    }
    else
    {
        return;
    }

    tCanPacket.u8len = (uint8_t)pCanRxMessage->DLC;
    if (tCanPacket.u8len != 0U)
    {
        memcpy(tCanPacket.au8data, pCanRxMessage->Data, tCanPacket.u8len);
    }

    u8WriteIndex = s_can_rx_write_index;
    u8NextWriteIndex = (uint8_t)((u8WriteIndex + 1U) % D_CAN_RX_QUEUE_DEPTH);
    if (u8NextWriteIndex == s_can_rx_read_index)
    {
        return;
    }

    s_can_rx_packets[u8WriteIndex] = tCanPacket;
    __DMB();
    s_can_rx_write_index = u8NextWriteIndex;
}

static void d_can_config_gpio(void)
{
    GPIO_InitType gpio_init;

    /* 原理图连接：CAN1_RX=PA11、CAN1_TX=PA12，使用CAN1默认复用。 */
    RCC_EnableAPB2PeriphClk(RCC_APB2_PERIPH_AFIO | RCC_APB2_PERIPH_GPIOA, ENABLE);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_11;
    gpio_init.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_InitPeripheral(GPIOA, &gpio_init);

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_12;
    gpio_init.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitPeripheral(GPIOA, &gpio_init);

    GPIO_ConfigPinRemap(GPIO_RMP1_CAN1, DISABLE);
    GPIO_ConfigPinRemap(GPIO_RMP2_CAN1, DISABLE);
}

static bool d_can_config_peripheral(void)
{
    CAN_InitType can_init;
    CAN_FilterInitType filter_init;
    RCC_ClocksType clocks;
    uint32_t baudrate_bps;
    uint32_t divisor;
    uint32_t prescaler;

    /* 根据PCLK1和固定时间量计算CAN1分频，当前项目波特率由项目参数统一配置。 */
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_CAN1, ENABLE);
    baudrate_bps = (uint32_t)APP_PARAM_CAN1_BAUDRATE_KBPS * 1000UL;
    divisor = baudrate_bps * D_CAN_TIME_QUANTA;
    if ((baudrate_bps == 0UL) || (divisor == 0UL))
    {
        return false;
    }

    RCC_GetClocksFreqValue(&clocks);
    if ((clocks.Pclk1Freq % divisor) != 0UL)
    {
        return false;
    }

    prescaler = clocks.Pclk1Freq / divisor;
    if ((prescaler == 0UL) || (prescaler > 1024UL))
    {
        return false;
    }

    CAN_DeInit(CAN1);

    CAN_InitStruct(&can_init);
    can_init.TTCM = DISABLE;
    can_init.ABOM = ENABLE;
    can_init.AWKUM = DISABLE;
    /* 保留CAN硬件自动重发；软件服务层会对长期未完成帧限时取消并最多补发一次。 */
    can_init.NART = DISABLE;
    can_init.RFLM = DISABLE;
    can_init.TXFP = ENABLE;
    can_init.OperatingMode = CAN_Normal_Mode;
    can_init.RSJW = CAN_RSJW_1tq;
    can_init.TBS1 = CAN_TBS1_14tq;
    can_init.TBS2 = CAN_TBS2_3tq;
    can_init.BaudRatePrescaler = (uint16_t)prescaler;

    if (CAN_Init(CAN1, &can_init) != CAN_InitSTS_Success)
    {
        CAN_DeInit(CAN1);
        return false;
    }

    /*
     * 过滤器0接收标准数据帧，过滤器1接收扩展数据帧。
     * 两类报文都进入FIFO0，由上层app_can统一判断是UDS还是DUCAR协议。
     */
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

    filter_init.Filter_Num = 1U;
    filter_init.Filter_LowId = 0x0004U;
    CAN1_InitFilter(&filter_init);

    CAN_ClearINTPendingBit(CAN1, CAN_INT_FOV0);
    CAN_ClearINTPendingBit(CAN1, CAN_INT_BOF);
    CAN_ClearINTPendingBit(CAN1, CAN_INT_LEC);
    CAN_ClearINTPendingBit(CAN1, CAN_INT_ERR);

    return true;
}

static void d_can_disable_interrupts(void)
{
    /* 屏蔽CAN中断并清除NVIC挂起，避免退出初始化时重入ISR。 */
    CAN_INTConfig(CAN1, CAN_INT_FMP0, DISABLE);
    CAN_INTConfig(CAN1, CAN_INT_FOV0, DISABLE);
    CAN_INTConfig(CAN1, CAN_INT_BOF, DISABLE);
    CAN_INTConfig(CAN1, CAN_INT_LEC, DISABLE);
    CAN_INTConfig(CAN1, CAN_INT_ERR, DISABLE);

    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_DisableIRQ(CAN1_SCE_IRQn);
    NVIC_ClearPendingIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_ClearPendingIRQ(CAN1_SCE_IRQn);
}

static void d_can_deinit_controller(void)
{
    CAN_DeInit(CAN1);
    RCC_EnableAPB1PeriphClk(RCC_APB1_PERIPH_CAN1, DISABLE);
}

static void d_can_set_pins_analog(void)
{
    GPIO_InitType gpio_init;

    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_11 | GPIO_PIN_12;
    gpio_init.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_InitPeripheral(GPIOA, &gpio_init);
}

static void d_can_disable_hardware(void)
{
    d_can_disable_interrupts();
    d_can_deinit_controller();
    d_can_set_pins_analog();
}

bool d_can_init(d_can_t *can)
{
    NVIC_InitType nvic_init;
    uint32_t primask;

    if (!d_can_is_instance_valid(can))
    {
        return false;
    }

    /* 初始化期间先屏蔽CAN中断，避免CAN外设尚未完成初始化就进入中断。 */
    NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_ClearPendingIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_DisableIRQ(CAN1_SCE_IRQn);
    NVIC_ClearPendingIRQ(CAN1_SCE_IRQn);

    if (s_can_ready != 0U)
    {
        d_can_deinit(can);
    }

    primask = __get_PRIMASK();
    __disable_irq();
    s_can_ready = 0U;
    s_can_rx_write_index = 0U;
    s_can_rx_read_index = 0U;
    memset(s_can_tx_mailbox_state, 0, sizeof(s_can_tx_mailbox_state));
    s_can_iap_silence_active = false;
    s_can_tx_fail_streak = 0U;
    s_can_bus_online = 1U;
    __set_PRIMASK(primask);

    d_can_config_gpio();
    if (!d_can_config_peripheral())
    {
        d_can_disable_hardware();
        return false;
    }

    /* 只有硬件、过滤器和队列状态都准备好后，才允许打开CAN中断。 */
    s_can_ready = 1U;

    nvic_init.NVIC_IRQChannel = USB_LP_CAN1_RX0_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 0U;
    nvic_init.NVIC_IRQChannelSubPriority = 0U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_ClearPendingIRQ(USB_LP_CAN1_RX0_IRQn);
    NVIC_Init(&nvic_init);

    nvic_init.NVIC_IRQChannel = CAN1_SCE_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 1U;
    nvic_init.NVIC_IRQChannelSubPriority = 0U;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_ClearPendingIRQ(CAN1_SCE_IRQn);
    NVIC_Init(&nvic_init);

    /* 先完成软件状态和NVIC配置，再开放CAN外设中断源。 */
    CAN_INTConfig(CAN1, CAN_INT_FMP0, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_FOV0, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_BOF, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_LEC, ENABLE);
    CAN_INTConfig(CAN1, CAN_INT_ERR, ENABLE);

    return true;
}

void d_can_deinit(d_can_t *can)
{
    uint32_t primask;

    if (!d_can_is_instance_valid(can))
    {
        return;
    }

    /* 先把ready置零，使并发进入的ISR立即退出，再关闭硬件。 */
    primask = __get_PRIMASK();
    __disable_irq();
    s_can_ready = 0U;
    __set_PRIMASK(primask);

    d_can_disable_hardware();
}

void d_can_prepare_low_power(d_can_t *can)
{
    GPIO_InitType gpio_init;
    uint32_t primask;

    if (!d_can_is_instance_valid(can))
    {
        return;
    }

    /* 先禁止新的CAN ISR/发送，再把TXD切为显性电平相反的隐性高电平。 */
    primask = __get_PRIMASK();
    __disable_irq();
    s_can_ready = 0U;
    __set_PRIMASK(primask);

    d_can_disable_interrupts();

    /* 先预置输出锁存器，再切换PA12复用，避免TXD短暂浮空。 */
    GPIO_WriteBit(GPIOA, GPIO_PIN_12, Bit_SET);
    GPIO_InitStruct(&gpio_init);
    gpio_init.Pin = GPIO_PIN_12;
    gpio_init.GPIO_Mode = GPIO_Mode_Out_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_2MHz;
    GPIO_InitPeripheral(GPIOA, &gpio_init);

    /* 保持TXD为隐性高电平，供低功耗流程先关闭隔离侧5V。 */
    d_can_deinit_controller();
}

void d_can_set_low_power_pins(void)
{
    d_can_set_pins_analog();
}

static bool d_can_send_internal(d_can_t *can,
                                uint32_t id,
                                d_can_id_t id_type,
                                const uint8_t *data,
                                uint8_t dlc,
                                uint8_t is_iap)
{
    CanTxMessage message;
    uint8_t mailbox;
    uint32_t primask;

    s_can_tx_request_count++;
    s_can_last_attempt_id = id;

    /* 先分别统计软件参数拒绝和CAN Bus-Off拒绝，避免混在邮箱不足中。 */
    if (!d_can_is_instance_valid(can) || (s_can_ready == 0U) ||
        ((id_type == D_CAN_ID_EXTENDED) && !s_extended_tx_enabled) ||
        (dlc > D_CAN_MAX_DATA_LEN) ||
        ((dlc != 0U) && (data == 0)) ||
        ((id_type != D_CAN_ID_STANDARD) && (id_type != D_CAN_ID_EXTENDED)) ||
        ((id_type == D_CAN_ID_STANDARD) && (id > D_CAN_STD_ID_MAX)) ||
        ((id_type == D_CAN_ID_EXTENDED) && (id > D_CAN_EXT_ID_MAX)))
    {
        s_can_tx_invalid_reject_count++;
        return false;
    }

    if (CAN_GetFlagSTS(CAN1, CAN_FLAG_BOFFL) != RESET)
    {
        s_can_tx_busoff_reject_count++;
        return false;
    }

    memset(&message, 0, sizeof(message));
    message.IDE = (id_type == D_CAN_ID_EXTENDED) ? CAN_Extended_Id : CAN_Standard_Id;
    message.RTR = CAN_RTRQ_Data;
    message.DLC = dlc;
    if (id_type == D_CAN_ID_EXTENDED)
    {
        message.ExtId = id;
    }
    else
    {
        message.StdId = id;
    }

    if (dlc != 0U)
    {
        memcpy(message.Data, data, dlc);
    }

    /* 关中断保护硬件邮箱申请，避免发送完成中断与邮箱状态读取竞争。 */
    primask = __get_PRIMASK();
    __disable_irq();
    if ((s_can_iap_silence_active != false) && (is_iap == 0U))
    {
        s_can_tx_invalid_reject_count++;
        __set_PRIMASK(primask);
        return false;
    }
    d_can_tx_collect_completed();
    mailbox = CAN_TransmitMessage(CAN1, &message);
    if (mailbox >= D_CAN_TX_MAILBOX_COUNT)
    {
        s_can_tx_no_mailbox_count++;
        __set_PRIMASK(primask);
        return false;
    }

    s_can_tx_activity_count++;
    s_can_tx_mailbox_accept_count++;
    s_can_tx_mailbox_state[mailbox].message = message;
    s_can_tx_mailbox_state[mailbox].start_ms = d_timer_get_ms();
    s_can_tx_mailbox_state[mailbox].retry_count = 0U;
    s_can_tx_mailbox_state[mailbox].active = 1U;
    s_can_tx_mailbox_state[mailbox].abort_requested = 0U;
    s_can_tx_mailbox_state[mailbox].is_iap = is_iap;
    __set_PRIMASK(primask);
    return true;
}

bool d_can_send(d_can_t *can,
                uint32_t id,
                d_can_id_t id_type,
                const uint8_t *data,
                uint8_t dlc)
{
    return d_can_send_internal(can, id, id_type, data, dlc, 0U);
}

bool d_can_send_iap(d_can_t *can,
                    uint32_t id,
                    d_can_id_t id_type,
                    const uint8_t *data,
                    uint8_t dlc)
{
    return d_can_send_internal(can, id, id_type, data, dlc, 1U);
}

void d_can_set_extended_tx_enabled(bool enabled)
{
    s_extended_tx_enabled = enabled;
}

bool d_can_is_tx_idle(d_can_t *can)
{
    uint32_t u32TxMailboxEmptyMask;

    /* 上层用于判断是否可以继续提交下一帧或结束一个发送阶段。 */
    if (!d_can_is_instance_valid(can) || (s_can_ready == 0U))
    {
        return false;
    }

    u32TxMailboxEmptyMask = CAN_TSTS_TMEM0 | CAN_TSTS_TMEM1 | CAN_TSTS_TMEM2;
    return ((can->handler->TSTS & u32TxMailboxEmptyMask) == u32TxMailboxEmptyMask);
}

bool d_can_is_bus_online(d_can_t *can)
{
    return d_can_is_instance_valid(can) && (s_can_ready != 0U) && (s_can_bus_online != 0U);
}

uint32_t d_can_get_rx_activity_count(void)
{
    return s_can_rx_activity_count;
}

uint32_t d_can_get_tx_activity_count(void)
{
    return s_can_tx_activity_count;
}

void d_can_get_tx_diagnostics(d_can_tx_diag_t *diagnostics)
{
    uint32_t primask;
    uint32_t u32TxStatus = 0U;

    if (diagnostics == 0)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();
    d_can_tx_collect_completed();

    diagnostics->request_count = s_can_tx_request_count;
    diagnostics->mailbox_accept_count = s_can_tx_mailbox_accept_count;
    diagnostics->no_mailbox_count = s_can_tx_no_mailbox_count;
    diagnostics->invalid_reject_count = s_can_tx_invalid_reject_count;
    diagnostics->busoff_reject_count = s_can_tx_busoff_reject_count;
    diagnostics->tx_complete_count = s_can_tx_complete_count;
    diagnostics->tx_failed_count = s_can_tx_failed_count;
    diagnostics->tx_retry_count = s_can_tx_retry_count;
    diagnostics->tx_terminal_fail_count = s_can_tx_terminal_fail_count;
    diagnostics->tx_timeout_count = s_can_tx_timeout_count;
    diagnostics->error_count = s_can_error_count;
    diagnostics->ack_error_count = s_can_ack_error_count;
    diagnostics->other_error_count = s_can_other_error_count;
    diagnostics->last_attempt_id = s_can_last_attempt_id;
    diagnostics->last_error_code = s_can_last_error_code;
    diagnostics->can_ready = s_can_ready;
    diagnostics->tx_fail_streak = s_can_tx_fail_streak;
    diagnostics->bus_online = s_can_bus_online;

    if (s_can_ready != 0U)
    {
        u32TxStatus = CAN1->TSTS;
        diagnostics->tx_error_counter = CAN_GetLSBTransmitErrCounter(CAN1);
        diagnostics->rx_error_counter = CAN_GetReceiveErrCounter(CAN1);
        diagnostics->bus_off = (CAN_GetFlagSTS(CAN1, CAN_FLAG_BOFFL) != RESET) ? 1U : 0U;
        diagnostics->error_passive = (CAN_GetFlagSTS(CAN1, CAN_FLAG_EPVFL) != RESET) ? 1U : 0U;
    }
    else
    {
        diagnostics->tx_error_counter = 0U;
        diagnostics->rx_error_counter = 0U;
        diagnostics->bus_off = 0U;
        diagnostics->error_passive = 0U;
    }

    diagnostics->tx_busy_mask = 0U;
    if ((u32TxStatus & CAN_TSTS_TMEM0) == 0U) diagnostics->tx_busy_mask |= 0x01U;
    if ((u32TxStatus & CAN_TSTS_TMEM1) == 0U) diagnostics->tx_busy_mask |= 0x02U;
    if ((u32TxStatus & CAN_TSTS_TMEM2) == 0U) diagnostics->tx_busy_mask |= 0x04U;
    __set_PRIMASK(primask);
}

bool d_can_is_rx_idle(d_can_t *can)
{
    if (!d_can_is_instance_valid(can) || (s_can_ready == 0U))
    {
        return false;
    }

    return (s_can_rx_read_index == s_can_rx_write_index) &&
           (CAN_PendingMessage(CAN1, CAN_FIFO0) == 0U);
}

bool d_can_read(t_can_packet *pCanPacket)
{
    uint8_t u8ReadIndex;

    /* 该函数只在任务上下文调用；复制完成后再移动读指针，保证数据完整。 */
    if (pCanPacket == 0)
    {
        return false;
    }

    u8ReadIndex = s_can_rx_read_index;
    if (u8ReadIndex == s_can_rx_write_index)
    {
        return false;
    }

    __DMB();
    *pCanPacket = s_can_rx_packets[u8ReadIndex];
    __DMB();
    s_can_rx_read_index = (uint8_t)((u8ReadIndex + 1U) % D_CAN_RX_QUEUE_DEPTH);
    return true;
}

void d_can_error_irq_handler(void)
{
    FlagStatus bus_off_flag;
    uint8_t u8ErrorCode;

    if (s_can_ready != 0U)
    {
        u8ErrorCode = CAN_GetLastErrCode(CAN1);
        s_can_last_error_code = u8ErrorCode;
        if (u8ErrorCode != CAN_ERRCode_NoErr)
        {
            s_can_error_count++;
            if (u8ErrorCode == CAN_ERRCode_ACKErr)
            {
                s_can_ack_error_count++;
            }
            else
            {
                s_can_other_error_count++;
            }
        }
    }

    /*
     * 外部刚使能SCE时可能残留错误请求。此时先重新初始化CAN，
     * 清掉初始化阶段产生的挂起状态，再按硬件状态清除对应请求源。
     */
    if (s_can_ready == 0U)
    {
        d_can_deinit(&can0);
        d_can_init(&can0);

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

void USB_LP_CAN1_RX0_IRQHandler(void)
{
    CanRxMessage message;
    uint8_t u8ProcessedFrames = 0U;

    /* RX中断只搬运FIFO数据；协议分发留给CAN_ReceiveTask()。 */
    if (s_can_ready == 0U)
    {
        /* 初始化/关闭窗口中不保留挂起的NVIC请求，避免中断风暴。 */
        NVIC_DisableIRQ(USB_LP_CAN1_RX0_IRQn);
        NVIC_ClearPendingIRQ(USB_LP_CAN1_RX0_IRQn);
        return;
    }

    /* FIFO0最多容纳3帧；限制单次ISR工作量，避免连续总线流量霸占启动线程。 */
    while ((u8ProcessedFrames < D_CAN_RX_ISR_MAX_FRAMES) &&
           (CAN_PendingMessage(CAN1, CAN_FIFO0) != 0U))
    {
        CAN_ReceiveMessage(CAN1, CAN_FIFO0, &message);
        (void)d_can_rx_data_handle(&message);
        u8ProcessedFrames++;
    }

    if (CAN_GetIntStatus(CAN1, CAN_INT_FOV0) != RESET)
    {
        CAN_ClearINTPendingBit(CAN1, CAN_INT_FOV0);
    }
}
