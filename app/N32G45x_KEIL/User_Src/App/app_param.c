/*
 * 文件作用：
 *   APP项目参数模块，集中承载BMS硬件拓扑、品号、成品条码、硬件版本、软件版本和出包版本，
 *   并负责制造SN和产品模式各自独立的Flash持久化。
 *
 * 主要用途：
 *   1. 给通信协议回复版本和品号。
 *   2. 给升级打包工具生成文件名和头包信息。
 *   3. 集中管理项目级常量，避免版本信息散落在多个模块。
 *   4. 在协议0x62写入SN后，为通信模块提供同一份运行时SN。
 *
 * 适用范围：
 *   适用于当前DUCAR N32G45x APP工程。不同客户、不同品号或不同出包版本优先修改本模块对应宏。
 *
 * 调用关系：
 *   main.c先调用app_param_init()，通信模块再通过app_param_get_xxx()读取参数；
 *   通信模块收到0x62后调用app_param_set_manufacture_sn()写入SN。
 *
 * 注意事项：
 *   修改品号或版本后必须重新编译APP并重新生成升级文件；默认品号A000000000用于强制升级场景。
 */
#include "App/app_param.h"
#include "App/app_param_api.h"
#include "App/app_param_validate.h"

#include "App/app_flash_map.h"
#include "Driver/d_flash.h"

#include <stddef.h>
#include <string.h>

/* Flash格式固定预留30个校准槽位，属于存储布局实现常量，不是项目可调参数。 */
#define APP_PARAM_CELL_VOLTAGE_CALIB_MAX_CELL_COUNT 30U

#pragma pack(push, 1)
typedef struct
{
    uint16_t tag;                                        /* 16位类型标识，Flash字节为“SN”。 */
    uint8_t sn[APP_BMS_MANUFACTURE_SN_MAX_LEN];          /* 固定50字节ASCII；末尾以0xFF填充。 */
} app_param_manufacture_sn_record_t;

typedef struct
{
    uint16_t tag;                                        /* Flash字节为“PM”。 */
    uint8_t mode;                                        /* 0生产/测试，1出货。 */
    uint8_t reserved;
} app_param_product_mode_record_t;

typedef struct
{
    uint16_t tag;                                        /* 16位类型标识，Flash字节为“VC”。 */
    int16_t offset_mv[APP_PARAM_CELL_VOLTAGE_CALIB_MAX_CELL_COUNT]; /* 单体偏差，单位mV。 */
} app_param_voltage_calibration_record_t;
#pragma pack(pop)

typedef char app_param_sn_record_must_fit_in_page[
    ((APP_FLASH_MAP_DB_PARAM_SN_OFFSET + sizeof(app_param_manufacture_sn_record_t)) ==
     APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET) ? 1 : -1];
typedef char app_param_mode_record_must_fit_in_page[
    ((APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET + sizeof(app_param_product_mode_record_t)) <=
     APP_FLASH_MAP_FLASH_PAGE_SIZE) ? 1 : -1];
typedef char app_param_voltage_record_must_fit_before_sn[
    ((APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET + sizeof(app_param_voltage_calibration_record_t)) <=
     APP_FLASH_MAP_DB_PARAM_SN_OFFSET) ? 1 : -1];

static char s_app_manufacture_sn[APP_BMS_MANUFACTURE_SN_MAX_LEN + 1U];
static int16_t s_app_cell_voltage_offset_mv[APP_PARAM_CELL_VOLTAGE_CALIB_MAX_CELL_COUNT];
/* 独立产品模式，保留全局符号便于Keil Watch查看。 */
uint8_t g_app_product_mode = APP_PARAM_PRODUCT_MODE_PRODUCTION;
/* Page0擦除前暂存整页，避免更新SN或校准记录时丢失同页内其他参数。 */
static uint32_t s_app_param_flash_page_buffer[APP_FLASH_MAP_FLASH_PAGE_SIZE / sizeof(uint32_t)];

/* SOC单体OCV表的唯一实体；索引0~100对应0%~100% SOC，单位mV。 */
const uint16_t g_app_param_soc_cell_ocv_table[101] = APP_PARAM_SOC_CELL_OCV_TABLE;

/* 更新Page0中的一条记录：整页读入RAM、仅修改目标字节、擦页并写回所有非空内容。 */
bool app_param_flash_page_update(uint32_t offset, const uint8_t *data, uint32_t len)
{
    volatile const uint32_t *pu32FlashPage;
    uint32_t u32WordCount;
    uint32_t u32WordIndex;
    uint32_t u32RunStart;
    uint32_t u32RunLength;
    bool bSuccess = false;

    if ((data == 0) || (len == 0U) || (offset > APP_FLASH_MAP_FLASH_PAGE_SIZE) ||
        (len > (APP_FLASH_MAP_FLASH_PAGE_SIZE - offset)))
    {
        return false;
    }

    if (!d_flash_acquire(D_FLASH_MUTEX_TIMEOUT_MS))
    {
        return false;
    }

    pu32FlashPage = (volatile const uint32_t *)APP_FLASH_MAP_DB_PARAM_ADDR;
    u32WordCount = APP_FLASH_MAP_FLASH_PAGE_SIZE / sizeof(uint32_t);
    for (u32WordIndex = 0U; u32WordIndex < u32WordCount; u32WordIndex++)
    {
        s_app_param_flash_page_buffer[u32WordIndex] = pu32FlashPage[u32WordIndex];
    }

    memcpy(((uint8_t *)s_app_param_flash_page_buffer) + offset, data, len);

    if (!d_flash_erase_page(APP_FLASH_MAP_DB_PARAM_ADDR))
    {
        goto app_param_flash_page_update_done;
    }

    /* 空白字保持擦除态不编程，其余相邻字组成连续写入段。 */
    u32WordIndex = 0U;
    while (u32WordIndex < u32WordCount)
    {
        if (s_app_param_flash_page_buffer[u32WordIndex] == 0xFFFFFFFFUL)
        {
            u32WordIndex++;
            continue;
        }

        u32RunStart = u32WordIndex;
        while ((u32WordIndex < u32WordCount) &&
               (s_app_param_flash_page_buffer[u32WordIndex] != 0xFFFFFFFFUL))
        {
            u32WordIndex++;
        }
        u32RunLength = (u32WordIndex - u32RunStart) * sizeof(uint32_t);

        if (!d_flash_write(APP_FLASH_MAP_DB_PARAM_ADDR + (u32RunStart * sizeof(uint32_t)),
                           (const uint8_t *)&s_app_param_flash_page_buffer[u32RunStart],
                           u32RunLength))
        {
            goto app_param_flash_page_update_done;
        }
    }

    for (u32WordIndex = 0U; u32WordIndex < u32WordCount; u32WordIndex++)
    {
        if (pu32FlashPage[u32WordIndex] != s_app_param_flash_page_buffer[u32WordIndex])
        {
            goto app_param_flash_page_update_done;
        }
    }

    bSuccess = true;

app_param_flash_page_update_done:
    d_flash_release();
    return bSuccess;
}

/* 校验制造SN是否为合法的可存储ASCII字符串。 */
static bool app_param_sn_is_valid(const uint8_t *pu8SnData, uint8_t u8SnLength)
{
    uint8_t u8SnIndex;

    if ((pu8SnData == 0) || (u8SnLength == 0U) || (u8SnLength > APP_BMS_MANUFACTURE_SN_MAX_LEN))
    {
        return false;
    }

    for (u8SnIndex = 0U; u8SnIndex < u8SnLength; u8SnIndex++)
    {
        if ((pu8SnData[u8SnIndex] < 0x20U) || (pu8SnData[u8SnIndex] > 0x7EU))
        {
            return false;
        }
    }

    return true;
}

/* 计算默认制造SN的有效长度。 */
static uint8_t app_param_default_sn_length(void)
{
    uint8_t u8DefaultSnLength = 0U;

    while ((u8DefaultSnLength < APP_BMS_MANUFACTURE_SN_MAX_LEN) &&
           (APP_BMS_MANUFACTURE_SN[u8DefaultSnLength] != '\0'))
    {
        u8DefaultSnLength++;
    }

    if ((u8DefaultSnLength == APP_BMS_MANUFACTURE_SN_MAX_LEN) &&
        (APP_BMS_MANUFACTURE_SN[u8DefaultSnLength] != '\0'))
    {
        return 0U;
    }

    if (!app_param_sn_is_valid((const uint8_t *)APP_BMS_MANUFACTURE_SN, u8DefaultSnLength))
    {
        return 0U;
    }

    return u8DefaultSnLength;
}

/* 更新运行时制造SN缓存并确保结尾保留字符串结束符。 */
static void app_param_set_runtime_sn(const uint8_t *pu8SnData, uint8_t u8SnLength)
{
    memset(s_app_manufacture_sn, 0, sizeof(s_app_manufacture_sn));
    if ((pu8SnData != 0) && (u8SnLength > 0U) && (u8SnLength <= APP_BMS_MANUFACTURE_SN_MAX_LEN))
    {
        memcpy(s_app_manufacture_sn, pu8SnData, u8SnLength);
    }
}

/* 将配置宏中的默认制造SN加载到运行时缓存。 */
static void app_param_load_default_sn(void)
{
    uint8_t u8DefaultSnLength;

    u8DefaultSnLength = app_param_default_sn_length();
    app_param_set_runtime_sn((const uint8_t *)APP_BMS_MANUFACTURE_SN, u8DefaultSnLength);
}

/* 识别清空后的SN：类型标识仍为“SN”，SN的50字节全部为擦除值。 */
static bool app_param_sn_record_is_cleared(const app_param_manufacture_sn_record_t *record)
{
    uint8_t u8SnIndex;

    if ((record == 0) || (record->tag != APP_FLASH_TAG_MANUFACTURE_SN))
    {
        return false;
    }

    for (u8SnIndex = 0U; u8SnIndex < APP_BMS_MANUFACTURE_SN_MAX_LEN; u8SnIndex++)
    {
        if (record->sn[u8SnIndex] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

/* 由固定50字节SN区的首个0xFF推导长度；0xFF之后不得再出现有效字符。 */
static bool app_param_sn_record_get_length(const app_param_manufacture_sn_record_t *record,
                                           uint8_t *length)
{
    uint8_t u8SnIndex;
    uint8_t u8Length = 0U;
    bool bPaddingStarted = false;

    if ((record == 0) || (length == 0) ||
        (record->tag != APP_FLASH_TAG_MANUFACTURE_SN))
    {
        return false;
    }

    for (u8SnIndex = 0U; u8SnIndex < APP_BMS_MANUFACTURE_SN_MAX_LEN; u8SnIndex++)
    {
        if (record->sn[u8SnIndex] == 0xFFU)
        {
            bPaddingStarted = true;
        }
        else
        {
            if (bPaddingStarted)
            {
                return false;
            }
            u8Length++;
        }
    }

    if (!app_param_sn_is_valid(record->sn, u8Length))
    {
        return false;
    }

    *length = u8Length;
    return true;
}

/* 初始化项目参数：按各自标识独立恢复模式、电压偏差和SN。 */
void app_param_init(void)
{
    const app_param_manufacture_sn_record_t *ptSnRecord;
    const app_param_product_mode_record_t *ptModeRecord;
    uint8_t u8SnLength;

    g_app_product_mode = APP_PARAM_PRODUCT_MODE_PRODUCTION;
    memset(s_app_manufacture_sn, 0, sizeof(s_app_manufacture_sn));
    memset(s_app_cell_voltage_offset_mv, 0, sizeof(s_app_cell_voltage_offset_mv));

    ptModeRecord = (const app_param_product_mode_record_t *)(
        APP_FLASH_MAP_DB_PARAM_ADDR + APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET);
    if ((ptModeRecord->tag == APP_FLASH_TAG_PRODUCT_MODE) &&
        (ptModeRecord->mode <= APP_PARAM_PRODUCT_MODE_SHIPPING))
    {
        g_app_product_mode = ptModeRecord->mode;
    }

    {
        const app_param_voltage_calibration_record_t *ptVoltageRecord =
            (const app_param_voltage_calibration_record_t *)(APP_FLASH_MAP_DB_PARAM_ADDR +
                                                              APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET);
        if (ptVoltageRecord->tag == APP_FLASH_TAG_VOLTAGE_CALIB)
        {
            memcpy(s_app_cell_voltage_offset_mv,
                   ptVoltageRecord->offset_mv,
                   sizeof(s_app_cell_voltage_offset_mv));
        }
    }

    ptSnRecord = (const app_param_manufacture_sn_record_t *)(APP_FLASH_MAP_DB_PARAM_ADDR +
                                                             APP_FLASH_MAP_DB_PARAM_SN_OFFSET);

    /* 清除SN后保留“SN”标识和全FF数据，模式仍由独立记录决定。 */
    if (app_param_sn_record_is_cleared(ptSnRecord))
    {
        app_param_set_runtime_sn(0, 0U);
        return;
    }
    if (app_param_sn_record_get_length(ptSnRecord, &u8SnLength))
    {
        app_param_set_runtime_sn(ptSnRecord->sn, u8SnLength);
        return;
    }

    /* 没有有效的“SN”标识时使用编译期默认值；Page3从不读取。 */
    app_param_load_default_sn();
}

uint8_t app_param_get_product_mode(void)
{
    return g_app_product_mode;
}

bool app_param_set_product_mode(uint8_t mode)
{
    app_param_product_mode_record_t record;
    const app_param_product_mode_record_t *saved =
        (const app_param_product_mode_record_t *)(
            APP_FLASH_MAP_DB_PARAM_ADDR + APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET);

    if (mode > APP_PARAM_PRODUCT_MODE_SHIPPING)
    {
        return false;
    }
    if ((mode == g_app_product_mode) &&
        (saved->tag == APP_FLASH_TAG_PRODUCT_MODE) &&
        (saved->mode == mode))
    {
        return true;
    }

    record.tag = APP_FLASH_TAG_PRODUCT_MODE;
    record.mode = mode;
    record.reserved = 0xFFU;
    if (!app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_PRODUCT_MODE_OFFSET,
                                     (const uint8_t *)&record, sizeof(record)))
    {
        return false;
    }

    g_app_product_mode = mode;
    return true;
}

/* 校验并持久化制造SN，产品模式不随SN写入变化。 */
bool app_param_set_manufacture_sn(const uint8_t *data, uint8_t len)
{
    app_param_manufacture_sn_record_t tSnRecord;

    if (!app_param_sn_is_valid(data, len))
    {
        return false;
    }

    memset(&tSnRecord, 0xFF, sizeof(tSnRecord));
    tSnRecord.tag = APP_FLASH_TAG_MANUFACTURE_SN;
    memcpy(tSnRecord.sn, data, len);

    if (!app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_SN_OFFSET,
                                     (const uint8_t *)&tSnRecord,
                                     (uint32_t)sizeof(tSnRecord)))
    {
        return false;
    }

    app_param_set_runtime_sn(data, len);
    return true;
}

/* 保留“SN”标识、把50字节SN数据全部写为FF；复位后识别为已清除。 */
bool app_param_clear_manufacture_sn(void)
{
    app_param_manufacture_sn_record_t tSnRecord;

    memset(&tSnRecord, 0xFF, sizeof(tSnRecord));
    tSnRecord.tag = APP_FLASH_TAG_MANUFACTURE_SN;

    if (!app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_SN_OFFSET,
                                     (const uint8_t *)&tSnRecord,
                                     (uint32_t)sizeof(tSnRecord)))
    {
        return false;
    }

    app_param_set_runtime_sn(0, 0U);
    return true;
}

/* 保存一节的有符号电压校准偏差；正值加到实测mV，负值从实测mV中扣除。 */
bool app_param_set_cell_voltage_offset(uint8_t cell_index, int16_t offset_mv)
{
    app_param_voltage_calibration_record_t tRecord;

    if (cell_index >= APP_PARAM_CELL_VOLTAGE_CALIB_MAX_CELL_COUNT)
    {
        return false;
    }

    tRecord.tag = APP_FLASH_TAG_VOLTAGE_CALIB;
    memcpy(tRecord.offset_mv,
           s_app_cell_voltage_offset_mv,
           sizeof(s_app_cell_voltage_offset_mv));
    tRecord.offset_mv[cell_index] = offset_mv;

    if (!app_param_flash_page_update(APP_FLASH_MAP_DB_PARAM_VOLTAGE_CALIB_OFFSET,
                                     (const uint8_t *)&tRecord,
                                     (uint32_t)sizeof(tRecord)))
    {
        return false;
    }

    s_app_cell_voltage_offset_mv[cell_index] = offset_mv;
    return true;
}

/* 返回指定电芯已保存的偏差；越界索引不访问Flash并返回0。 */
int16_t app_param_get_cell_voltage_offset(uint8_t cell_index)
{
    if (cell_index >= APP_PARAM_CELL_VOLTAGE_CALIB_MAX_CELL_COUNT)
    {
        return 0;
    }

    return s_app_cell_voltage_offset_mv[cell_index];
}

/* 对单体电压叠加保存的偏差，并限制在uint16_t范围。 */
uint16_t app_param_apply_cell_voltage_offset(uint8_t cell_index, uint16_t measured_mv)
{
    int32_t s32CalibratedMv;

    s32CalibratedMv = (int32_t)measured_mv + (int32_t)app_param_get_cell_voltage_offset(cell_index);
    if (s32CalibratedMv < 0L)
    {
        return 0U;
    }
    if (s32CalibratedMv > 65535L)
    {
        return 65535U;
    }

    return (uint16_t)s32CalibratedMv;
}

/* 获取软件品号配置。 */
const char *app_param_get_software_pn(void)
{
    return APP_PARAM_SOFTWARE_PN;
}

/* 获取当前运行时制造SN。 */
const char *app_param_get_manufacture_sn(void)
{
    return s_app_manufacture_sn;
}

/* 获取电池/平台型号配置。 */
const char *app_param_get_bat_type(void)
{
    return APP_PARAM_BAT_TYPE;
}

/* 获取硬件版本配置。 */
const char *app_param_get_hw_version(void)
{
    return APP_PARAM_HW_VERSION;
}

/* 获取出包T版本配置。 */
const char *app_param_get_package_t_version(void)
{
    return APP_PARAM_PACKAGE_T_VERSION;
}

/* 获取出包V版本配置。 */
const char *app_param_get_package_v_version(void)
{
    return APP_PARAM_PACKAGE_V_VERSION;
}
