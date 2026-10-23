#ifndef __D_CAN_H__
#define __D_CAN_H__

#include <stdbool.h>
#include <stdint.h>

#define D_CAN_MAX_DATA_LEN ((uint8_t)8U)
/*
 * 接收环形队列深度。中断只负责快速入队，协议任务负责出队；
 * 实际可用容量比宏值少1个槽位，用于区分“空”和“满”。
 */
#define D_CAN_RX_QUEUE_DEPTH 256U

typedef enum
{
    D_CAN_ID_STANDARD = 0U,
    D_CAN_ID_EXTENDED
} d_can_id_t;

typedef struct d_can_t d_can_t;

/*
 * 一帧完整的原始CAN数据。该结构只描述硬件收到的内容，
 * 不携带普通协议、IAP或UDS的业务含义。
 */
typedef struct
{
    uint32_t u32id;
    uint32_t u32idType;
    uint8_t u8len;
    uint8_t au8data[D_CAN_MAX_DATA_LEN];
} t_can_packet;

/* CAN发送路径的累计诊断快照，仅供低频RTT定位邮箱拥塞和链路错误。 */
typedef struct
{
    uint32_t request_count;
    uint32_t mailbox_accept_count;
    uint32_t no_mailbox_count;
    uint32_t invalid_reject_count;
    uint32_t busoff_reject_count;
    uint32_t tx_complete_count;
    uint32_t tx_failed_count;
    uint32_t tx_retry_count;
    uint32_t tx_terminal_fail_count;
    uint32_t tx_timeout_count;
    uint32_t error_count;
    uint32_t ack_error_count;
    uint32_t other_error_count;
    uint32_t last_attempt_id;
    uint8_t last_error_code;
    uint8_t tx_error_counter;
    uint8_t rx_error_counter;
    uint8_t tx_busy_mask;
    uint8_t bus_off;
    uint8_t error_passive;
    uint8_t can_ready;
    uint8_t tx_fail_streak;
    uint8_t bus_online;
} d_can_tx_diag_t;

extern d_can_t can0;

bool d_can_init(d_can_t *can);
void d_can_deinit(d_can_t *can);
/* 停止CAN并将TX保持隐性高电平，等待收发器掉电后再切换低功耗引脚。 */
void d_can_prepare_low_power(d_can_t *can);
void d_can_set_low_power_pins(void);

/* UDS CommunicationControl gates extended DUCAR traffic, not standard UDS IDs. */
void d_can_set_extended_tx_enabled(bool enabled);

/*
 * 将一帧数据直接提交到CAN硬件发送邮箱。
 * 返回true只表示硬件邮箱接收成功，不表示报文已经在线路上完成发送。
 * 底层驱动不做软件发送队列；普通邮箱发生错误时最多重试一次。
 * CAN IAP静默期间普通发送会被拒绝，IAP响应使用专用发送入口。
 */
bool d_can_send(d_can_t *can,
                uint32_t id,
                d_can_id_t id_type,
                const uint8_t *data,
                uint8_t dlc);
/* IAP专用发送入口：升级静默期间仍允许ACK/CTS/EOM等升级应答发送。 */
bool d_can_send_iap(d_can_t *can,
                    uint32_t id,
                    d_can_id_t id_type,
                    const uint8_t *data,
                    uint8_t dlc);
/* 静默普通CAN并取消已占用的普通发送邮箱；IAP专用发送不受影响。 */
void d_can_set_iap_silence(bool enabled);

/* 查询3个硬件发送邮箱是否全部空闲。 */
bool d_can_is_tx_idle(d_can_t *can);
/* 近期发送成功或收到总线帧时在线；连续发送失败后抑制周期报文。 */
bool d_can_is_bus_online(d_can_t *can);
/* 每个通信任务周期调用，清理完成状态并超时取消长期未完成的发送。 */
void d_can_service_tx(void);

/* 统计所有硬件接收帧（包括未分发/队列满）和成功提交邮箱的发送帧。 */
uint32_t d_can_get_rx_activity_count(void);
uint32_t d_can_get_tx_activity_count(void);
void d_can_get_tx_diagnostics(d_can_tx_diag_t *diagnostics);

/* 软件接收队列和硬件FIFO0都为空时才允许停止CAN。 */
bool d_can_is_rx_idle(d_can_t *can);

/* 从接收环形队列复制一帧原始报文；队列为空时返回false。 */
bool d_can_read(t_can_packet *pCanPacket);

/* CAN1系统错误中断钩子，由MCU中断向量调用。 */
void d_can_error_irq_handler(void);

#endif
