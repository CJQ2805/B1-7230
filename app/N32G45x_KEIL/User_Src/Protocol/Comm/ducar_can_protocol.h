#ifndef __DUCAR_CAN_PROTOCOL_H__
#define __DUCAR_CAN_PROTOCOL_H__

/*
 * 文件作用：DUCAR CAN普通协议的公共定义。
 *
 * 这里集中放置29位扩展帧的地址、PF、PGN、ACK/NAK和ID拆分结构，
 * 供APP普通协议、CAN IAP以及BOOT CAN协议共同使用。这里只定义协议，
 * 不实现收发；具体组包、解析和状态机分别在各自的.c文件中完成。
 */

#include <stdbool.h>
#include <stdint.h>

#include "Driver/d_can.h"

/* DUCAR CAN普通协议固定使用CAN 2.0B扩展帧，数据区最多8字节。 */
#define DUCAR_CAN_MAX_DATA_LEN          ((uint8_t)8U)
#define DUCAR_CAN_EXT_ID_MASK           ((uint32_t)0x1FFFFFFFUL)
#define DUCAR_CAN_DEFAULT_PRIORITY      ((uint8_t)6U)
/*
 * 当前固件实例的BMS源地址。默认PACK地址为0x16；构建其他PACK固件时，
 * 可通过编译器预定义DUCAR_CAN_BMS_A_ADDRESS覆盖，例如设为0x17。
 */
#ifndef DUCAR_CAN_BMS_A_ADDRESS
#define DUCAR_CAN_BMS_A_ADDRESS         0x16U
#endif
#define DUCAR_CAN_PC_ADDRESS            ((uint8_t)0x3AU)
#define DUCAR_CAN_GLOBAL_ADDRESS        ((uint8_t)0xFFU)

/*
 * 协议使用的PDU格式和PF：
 * EA为请求，E8为ACK/NAK，EC/EB为TP控制/数据，E1为CAN IAP控制，
 * F2为RTC等按PGN处理的普通功能，09/08/E0为普通控制功能。
 */
#define DUCAR_CAN_PF_REQUEST            ((uint8_t)0xEAU)
#define DUCAR_CAN_PF_ACK                ((uint8_t)0xE8U)
#define DUCAR_CAN_PF_TP_CM              ((uint8_t)0xECU)
#define DUCAR_CAN_PF_TP_DT              ((uint8_t)0xEBU)

#define DUCAR_CAN_PF_F2                 ((uint8_t)0xF2U)
#define DUCAR_CAN_PF_RESET              ((uint8_t)0x08U) /* 0800：Data0基础控制，Data1产品模式。 */
#define DUCAR_CAN_PF_MOS_CONTROL        ((uint8_t)0x09U)
#define DUCAR_CAN_PF_STANDBY_CURRENT    ((uint8_t)0x0AU)
#define DUCAR_CAN_PF_DEBUG_V            ((uint8_t)0x0BU)
#define DUCAR_CAN_PF_DEBUG_CELL         ((uint8_t)0x0CU)
#define DUCAR_CAN_PF_CAN_CONTROL        ((uint8_t)0xE0U)
#define DUCAR_CAN_PF_CURRENT_CALIB      ((uint8_t)0xE7U)
#define DUCAR_CAN_PF_SOC_WRITE          ((uint8_t)0xE2U)
#define DUCAR_CAN_PF_CLEAR_SN           ((uint8_t)0xE3U) /* E300：PC到BMS，ID模板18 E3 16 SA。 */
#define DUCAR_CAN_PF_BOOT_CONTROL       ((uint8_t)0xE1U)

/* 普通协议主动报文和按需查询报文的PGN。 */
#define DUCAR_CAN_PGN_F20D              ((uint16_t)0xF20DU)
#define DUCAR_CAN_PGN_F20E              ((uint16_t)0xF20EU)
#define DUCAR_CAN_PGN_F20F              ((uint16_t)0xF20FU)
#define DUCAR_CAN_PGN_F210              ((uint16_t)0xF210U)
#define DUCAR_CAN_PGN_F211              ((uint16_t)0xF211U)
#define DUCAR_CAN_PGN_F212              ((uint16_t)0xF212U)
#define DUCAR_CAN_PGN_F213              ((uint16_t)0xF213U)
#define DUCAR_CAN_PGN_F214              ((uint16_t)0xF214U)
#define DUCAR_CAN_PGN_F215              ((uint16_t)0xF215U)
#define DUCAR_CAN_PGN_F216              ((uint16_t)0xF216U)
#define DUCAR_CAN_PGN_F217              ((uint16_t)0xF217U)
#define DUCAR_CAN_PGN_F218              ((uint16_t)0xF218U)
#define DUCAR_CAN_PGN_F21A              ((uint16_t)0xF21AU)
#define DUCAR_CAN_PGN_F21B              ((uint16_t)0xF21BU)
#define DUCAR_CAN_PGN_F21C              ((uint16_t)0xF21CU)
#define DUCAR_CAN_PGN_F21D              ((uint16_t)0xF21DU)
#define DUCAR_CAN_PGN_F21E              ((uint16_t)0xF21EU)
#define DUCAR_CAN_PGN_F21F              ((uint16_t)0xF21FU)
/* A0.2：F219改为充电需求主动报文，不再表示RTC。 */
#define DUCAR_CAN_PGN_F219              ((uint16_t)0xF219U)
/* A0.2：主动告警使用F21A~F21C，主动保护使用F21D~F21F。 */
#define DUCAR_CAN_PGN_F22E              ((uint16_t)0xF22EU)
#define DUCAR_CAN_PGN_F22F              ((uint16_t)0xF22FU)
#define DUCAR_CAN_PGN_F220              ((uint16_t)0xF220U)
#define DUCAR_CAN_PGN_F221              ((uint16_t)0xF221U)
#define DUCAR_CAN_PGN_F200              ((uint16_t)0xF200U)
#define DUCAR_CAN_PGN_F201              ((uint16_t)0xF201U)
#define DUCAR_CAN_PGN_F203              ((uint16_t)0xF203U)
#define DUCAR_CAN_PGN_F205              ((uint16_t)0xF205U)
#define DUCAR_CAN_PGN_F206              ((uint16_t)0xF206U)
#define DUCAR_CAN_PGN_F208              ((uint16_t)0xF208U)
#define DUCAR_CAN_PGN_F20B              ((uint16_t)0xF20BU)
#define DUCAR_CAN_PGN_F20C              ((uint16_t)0xF20CU)
/* A0.2：RTC查询和写入统一使用F229。 */
#define DUCAR_CAN_PGN_F229              ((uint16_t)0xF229U)
#define DUCAR_CAN_PGN_F230              ((uint16_t)0xF230U)
#define DUCAR_CAN_PGN_F231              ((uint16_t)0xF231U)
#define DUCAR_CAN_PGN_F232              ((uint16_t)0xF232U)
#define DUCAR_CAN_PGN_F233              ((uint16_t)0xF233U)
#define DUCAR_CAN_PGN_E200              ((uint16_t)0xE200U)

/* CAN控制响应中的ACK/NAK。 */
#define DUCAR_CAN_ACK                   ((uint8_t)0x00U)
#define DUCAR_CAN_NAK                   ((uint8_t)0x01U)

typedef struct
{
    uint8_t u8Priority;                  /* 29位CAN ID的优先级。 */
    uint8_t u8Reserved;                  /* 29位CAN ID的保留位。 */
    uint8_t u8DataPage;                  /* 29位CAN ID的数据页位。 */
    uint8_t u8Pf;                        /* PDU Format。 */
    uint8_t u8Ps;                        /* PDU Specific / 目标地址。 */
    uint8_t u8Sa;                        /* 源地址。 */
} ducar_can_id_fields_t;

/* 普通协议有限发送槽状态，供CAN低频诊断日志查看。 */
typedef struct
{
    uint8_t cal2_reply_count;
    uint8_t ack_pending;
    uint8_t tp_state;
    uint8_t tp_next_packet;
    uint8_t tp_packet_count;
} ducar_can_tx_queue_diag_t;

/* 初始化普通协议内部计数器、TP状态、ACK状态和DETD主动发送窗口。 */
void ducar_can_protocol_init(void);
/* IAP活动期间暂停普通CAN发送，并在进入静默时清除旧的待发送业务报文。 */
void ducar_can_protocol_set_iap_silence(bool enabled);
/* 从全局快照刷新DETD沿和一次性主动发送窗口。 */
void ducar_can_data_renew(void);
/* 每1ms维护调度时基、LifeValue和TP超时；与Comm_Task调用周期一致。 */
void ducar_can_bus_timer_handle(void);
/* 消耗一次DETD触发的主动发送窗口，并返回本周期是否允许发送。 */
bool ducar_can_detd_tx_window_is_active(void);
/* 调度50ms报文F218。 */
void ducar_can_50ms_task(void);
/* 按GD7627节拍槽调度100ms报文。 */
void ducar_can_100ms_task(void);
/* 按GD7627节拍槽调度200ms报文。 */
void ducar_can_200ms_task(void);
/* 按项目节拍槽调度500ms报文。 */
void ducar_can_500ms_task(void);
/* 按1000ms周期发送F220。 */
void ducar_can_1000ms_task(void);
/* 接收一帧普通扩展帧，按PF分发到请求、TP或控制处理函数。 */
void ducar_can_protocol_receive(const t_can_packet *ptCanPacket);
/* 发送排队的ACK/NAK，并在ACK发送完成后执行复位或关机动作。 */
void ducar_can_control_reply_task(void);
/* CAL2活跃或仍有待发送扩展应答时禁止STOP。 */
bool ducar_can_cal2_allows_sleep(void);
void ducar_can_get_tx_queue_diagnostics(ducar_can_tx_queue_diag_t *diagnostics);

/* 判断29位CAN ID的源地址是否为上位机PC地址0x3A。 */
bool ducar_can_id_is_pc_source(const ducar_can_id_fields_t *ptCanIdFields);

#endif
