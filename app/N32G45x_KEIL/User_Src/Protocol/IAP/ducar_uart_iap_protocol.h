#ifndef __DUCAR_UART_IAP_PROTOCOL_H__
#define __DUCAR_UART_IAP_PROTOCOL_H__

#include <stdbool.h>
#include <stdint.h>

/*
 * 文件作用：
 *   定义并承载当前APP使用的DUCAR UART IAP协议接口、常量、数据类型和业务入口。
 *
 * 目录归属：
 *   本文件是工程本地协议模块。APP版本位于app/User/Protocol，BOOT版本位于
 *   boot/User/Protocol，两边不依赖公共目录，但协议字段变更时必须同步修改。
 *
 * 使用方法：
 *   本模块负责UART IAP解析/组帧、CRC/元数据校验、APP2升级状态机、UART分流。
 */

/* DUCAR串口升级协议固定字段：AA 01 CMD LEN DATA SUM 3A 4B。 */
#define DUCAR_IAP_SOF                 ((uint8_t)0xAAU)
#define DUCAR_IAP_ADDR                ((uint8_t)0x01U)
#define DUCAR_IAP_EOF0                ((uint8_t)0x3AU)
#define DUCAR_IAP_EOF1                ((uint8_t)0x4BU)

/* DUCAR串口升级协议命令字。 */
#define DUCAR_IAP_CMD_CHECK_IN        ((uint8_t)0x2AU)
#define DUCAR_IAP_CMD_UPDATE_START    ((uint8_t)0x01U)
#define DUCAR_IAP_CMD_PROGRAM         ((uint8_t)0x02U)
#define DUCAR_IAP_CMD_JUMP            ((uint8_t)0x03U)
#define DUCAR_IAP_CMD_CHECK           ((uint8_t)0x04U)

/* 协议应答和当前运行区域标识。 */
#define DUCAR_IAP_ACK                 ((uint8_t)0x00U)
#define DUCAR_IAP_NAK                 ((uint8_t)0x01U)
#define DUCAR_IAP_IN_APP              ((uint8_t)0x00U)
#define DUCAR_IAP_IN_BOOT             ((uint8_t)0x01U)

/* 当前协议固定每个Program数据页为128字节。 */
#define DUCAR_IAP_PAGE_SIZE           ((uint16_t)128U)
#define DUCAR_IAP_PROGRAM_DATA_LEN    ((uint8_t)(2U + DUCAR_IAP_PAGE_SIZE))
#define DUCAR_IAP_MAX_DATA_LEN        ((uint16_t)255U)
#define DUCAR_IAP_MAX_FRAME_LEN       ((uint16_t)(1U + 1U + 1U + 1U + DUCAR_IAP_MAX_DATA_LEN + 1U + 2U))

/* 第0帧校验数据在128字节DATA区里的固定偏移。 */
#define DUCAR_IAP_META_MARK_OFFSET    ((uint16_t)0U)
#define DUCAR_IAP_META_MARK_LEN       ((uint16_t)7U)
#define DUCAR_IAP_META_PART_OFFSET    ((uint16_t)17U)
#define DUCAR_IAP_META_PART_LEN       ((uint16_t)10U)
#define DUCAR_IAP_META_CRC_OFFSET     ((uint16_t)31U)
#define DUCAR_IAP_META_SIZE_OFFSET    ((uint16_t)35U)
#define DUCAR_IAP_DEFAULT_PART_NUMBER "A000000000"

typedef enum
{
    DUCAR_IAP_PARSE_NONE = 0,   /* 当前字节还没有组成完整帧。 */
    DUCAR_IAP_PARSE_OK,         /* 已经收到一帧完整且校验正确的数据。 */
    DUCAR_IAP_PARSE_ERROR       /* 已经收到一帧异常数据，调用者可以按命令回复NAK。 */
} ducar_iap_parse_result_t;

typedef struct
{
    uint8_t cmd;                         /* 命令字。 */
    uint8_t len;                         /* DATA长度。 */
    uint8_t data[DUCAR_IAP_MAX_DATA_LEN];   /* DATA内容。 */
} ducar_iap_frame_t;

typedef struct
{
    uint8_t state;                       /* 当前解析状态。 */
    uint8_t data_index;                  /* DATA已经接收的字节数。 */
    uint8_t checksum;                    /* 本帧实时累加校验和，按uint8_t自然截断。 */
    ducar_iap_frame_t frame;             /* 正在解析的帧。 */
} ducar_iap_parser_t;

typedef struct
{
    uint16_t page_count;                 /* Update Start命令携带的页数量。 */
    uint16_t page_size;                  /* Update Start命令携带的页大小。 */
} ducar_iap_update_info_t;

typedef struct
{
    uint32_t crc32;                      /* 固件区CRC32，低字节在前解析成CPU字节序。 */
    uint32_t size;                       /* 固件包真实字节数，不包含第0帧元数据。 */
    char part_number[DUCAR_IAP_META_PART_LEN + 1U]; /* 固件品号，末尾补'\0'方便调试查看。 */
} ducar_iap_firmware_info_t;

/* 初始化UART IAP字节流解析器。 */
void ducar_iap_parser_init(ducar_iap_parser_t *parser);
/* 向UART IAP解析器输入一个字节并尝试生成完整帧。 */
ducar_iap_parse_result_t ducar_iap_parser_input(ducar_iap_parser_t *parser, uint8_t byte, ducar_iap_frame_t *out_frame);
/* 按UART IAP格式组装一帧协议报文。 */
uint16_t ducar_iap_pack_frame(uint8_t cmd, const uint8_t *data, uint8_t len, uint8_t *out_frame, uint16_t out_size);
/* 读取UART IAP数据帧中的帧序号。 */
uint16_t ducar_iap_get_frame_no(const ducar_iap_frame_t *frame);
/* 解析Update Start帧中的升级参数。 */
bool ducar_iap_parse_update_start(const ducar_iap_frame_t *frame, ducar_iap_update_info_t *info);
/* 解析固件元数据帧中的CRC、长度和品号。 */
bool ducar_iap_parse_firmware_info(const ducar_iap_frame_t *frame, ducar_iap_firmware_info_t *info);
/* 判断固件品号是否允许在当前设备上升级。 */
bool ducar_iap_part_number_is_allowed(const char *local_part_number, const char *firmware_part_number);
/* 根据固件大小计算需要传输的数据帧数。 */
uint16_t ducar_iap_calc_data_frames(uint32_t firmware_size);
/* 计算数据缓冲区的CRC32。 */
uint32_t ducar_iap_crc32_calc(const uint8_t *data, uint32_t len);

/* 初始化UART IAP业务协议模块。 */
void ducar_iap_protocol_init(void);
/* 执行UART IAP协议周期处理。 */
void ducar_iap_protocol_task(void);
/* 判断UART IAP协议链路是否在线。 */
uint8_t ducar_iap_protocol_is_online(void);

/* 判断命令字是否属于UART IAP协议。 */
bool ducar_iap_is_protocol_cmd(uint8_t cmd);
/* 处理一帧已完成校验的UART IAP报文。 */
void ducar_iap_handle_frame(const ducar_iap_frame_t *frame);
/* 处理UART IAP报文解析错误。 */
void ducar_iap_handle_parse_error(uint8_t cmd);
/* 保存APP镜像长度/CRC，并准备将APP升级数据交给BOOT搬运。 */
bool ducar_iap_prepare_boot_move(uint32_t image_size, uint32_t image_crc32);
/* CAN IAP切入BOOT等待CAN升级包：保存PF并写入BBBB升级状态，不擦写APP2。 */
bool ducar_iap_prepare_boot_wait_package(void);
/* 回滚未能完成UDS肯定响应的BOOT等待状态，避免下次掉电误留在BOOT。 */
bool ducar_iap_cancel_boot_wait_package(void);
/* 判断UART IAP是否正在升级。 */
bool ducar_iap_is_busy(void);

#endif
