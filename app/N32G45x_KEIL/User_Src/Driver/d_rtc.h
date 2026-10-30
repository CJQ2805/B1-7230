#ifndef _D_RTC_H_
#define _D_RTC_H_

#include <stdbool.h> /* 使用bool类型作为接口返回值。 */
#include <stdint.h>  /* 使用uint8_t和uint16_t等固定宽度整数。 */

#define Module_Major_Version_D_RTC    0 /* RTC底层驱动主版本号。 */
#define Module_Minor_Version_D_RTC    1 /* RTC底层驱动次版本号。 */

typedef struct
{
    uint16_t year;  /* 年份，范围2000~2099。 */
    uint8_t month;  /* 月份，范围1~12。 */
    uint8_t day;    /* 日期，范围1~31，会按月份和闰年检查。 */
    uint8_t week;   /* 星期，0表示写入时自动计算，1~7表示周一~周日。 */
    uint8_t hour;   /* 小时，24小时制，范围0~23。 */
    uint8_t min;    /* 分钟，范围0~59。 */
    uint8_t sec;    /* 秒，范围0~59。 */
} d_rtc_time_t;

bool d_rtc_init(void);                    /* 初始化RTC硬件，使用32.768kHz外部LSE晶振。 */
bool d_rtc_write(const d_rtc_time_t *time); /* 写入真实年月日时分秒，成功后RTC时间才会被标记为有效。 */
bool d_rtc_read(d_rtc_time_t *time);      /* 读取RTC真实时间；没有写过有效时间时返回false。 */
bool d_rtc_read_calendar(d_rtc_time_t *time); /* 读取RTC当前日历，不检查“已校时”备份标志。 */
bool d_rtc_read_seconds_of_day(uint32_t *seconds); /* 读取RTC时分秒计数；用于短周期相对超时，不要求已校时。 */
bool d_rtc_read_seconds_since_2000(uint32_t *seconds); /* RTC日历转秒数，包含跨日/月/年；时钟回拨由调用方检测。 */
bool d_rtc_is_valid(void);                /* 查询RTC时间是否已经写入过有效值。 */

bool d_rtc_wakeup_start(uint16_t seconds); /* 启动RTC自动唤醒，seconds按1Hz RTC节拍计数。 */
void d_rtc_wakeup_stop(void);              /* 关闭RTC自动唤醒，并清除RTC/EXTI20挂起标志。 */
bool d_rtc_wakeup_is_pending(void);        /* 查询RTC唤醒软件标志，用来区分本次唤醒来源。 */
void d_rtc_wakeup_clear_pending(void);     /* 清除RTC唤醒软件标志。 */
void d_rtc_wakeup_irq_handler(void);       /* RTC_WKUP_IRQHandler中调用，内部只清标志不做耗时动作。 */

#endif
