/*
 * 文件作用：
 *   N32G45X RTC驱动实现，使用外部低速晶振提供时间读写和低功耗唤醒能力。
 *
 * 主要用途：
 *   1. 初始化RTC基础计时。
 *   2. 提供时间读写接口给LOG和通信模块。
 *   3. 配置RTC唤醒中断作为STOP模式主动唤醒源。
 *
 * 适用范围：
 *   适用于配有32.768kHz外部RTC晶振的N32G45X硬件。更换MCU或RTC时保留 d_rtc.h，对本文件重新实现。
 *
 * 调用关系：
 *   main.c 初始化RTC；LOG、通信、低功耗模块调用 d_rtc_xxx()。
 *
 * 注意事项：
 *   当前使用LSE；掉电无备份电源时绝对时间可能失效。
 */
#include "Driver/d_rtc.h" /* RTC底层封装对外接口和时间结构体定义。 */
#include "Driver/d_timer.h" /* 以初始化后的1ms时基限制LSE起振等待时间。 */

#include "n32g45x_bkp.h"   /* 备份寄存器接口，用来保存RTC是否已设置过的标记。 */
#include "n32g45x_exti.h"  /* RTC WakeUp通过EXTI20唤醒STOP模式。 */
#include "n32g45x_pwr.h"   /* 电源/备份域访问接口，访问RTC备份域前必须打开权限。 */
#include "n32g45x_rcc.h"   /* RCC时钟接口，用来开启LSE并选择RTC时钟源。 */
#include "n32g45x_rtc.h"   /* 国民标准库RTC日历读写接口。 */
#include "misc.h"           /* 配置RTC_IRQn中断优先级。 */

#define D_RTC_BKP_MARK_REG        BKP_DAT1      /* 使用备份寄存器1保存“RTC时间已写入”的标记。 */
#define D_RTC_BKP_MARK_VALUE      ((uint16_t)0xA55AU) /* 标记值，读到这个值才认为RTC时间可信。 */
#define D_RTC_LSE_READY_TIMEOUT_MS ((uint32_t)5000UL) /* 外部晶振起振最多等待5秒。 */
#define D_RTC_LSE_ASYNC_PREDIV     ((uint32_t)127U) /* 32768 / (128 * 256) = 1Hz。 */
#define D_RTC_LSE_SYNC_PREDIV      ((uint32_t)255U)

static uint8_t s_d_rtc_inited;      /* 本次上电后RTC底层是否已经初始化成功。 */
static uint8_t s_d_rtc_init_failed; /* 普通读取时是否已经初始化失败过，防止LOG任务反复等待超时。 */

/* 判断年份是否为闰年，写RTC日期前要用它计算2月份最大天数。 */
static bool d_rtc_is_leap_year(uint16_t year)
{
    if ((year % 400U) == 0U) /* 能被400整除的年份一定是闰年。 */
    {
        return true;         /* 返回闰年。 */
    }

    if ((year % 100U) == 0U) /* 能被100整除但不能被400整除的年份不是闰年。 */
    {
        return false;        /* 返回平年。 */
    }

    return ((year % 4U) == 0U); /* 其他年份能被4整除就是闰年。 */
}

/* 返回某一年某个月的最大日期，避免写入不存在的日期。 */
static uint8_t d_rtc_get_month_days(uint16_t year, uint8_t month)
{
    static const uint8_t days_table[12] = /* 平年每个月对应的天数。 */
    {
        31U, 28U, 31U, 30U, 31U, 30U,
        31U, 31U, 30U, 31U, 30U, 31U
    };

    if ((month == 2U) && d_rtc_is_leap_year(year)) /* 闰年的2月份有29天。 */
    {
        return 29U;                                /* 返回闰年2月份天数。 */
    }

    return days_table[month - 1U];                 /* 其他月份直接查表返回。 */
}

/* 根据年月日计算星期，返回1~7，对应周一~周日。 */
static uint8_t d_rtc_calc_week(uint16_t year, uint8_t month, uint8_t day)
{
    uint16_t u16CalculationYear = year;    /* 用临时年份参与公式计算，避免修改入参。 */
    uint8_t u8CalculationMonth = month;   /* 用临时月份参与公式计算，避免修改入参。 */
    uint16_t u16YearLowPart;              /* 年份的后两位。 */
    uint16_t u16YearHighPart;             /* 年份的前两位。 */
    uint16_t u16WeekFormulaValue;         /* 蔡勒公式计算出的星期中间值。 */

    if (u8CalculationMonth < 3U)          /* 公式中1月和2月按上一年的13月和14月处理。 */
    {
        u8CalculationMonth = (uint8_t)(u8CalculationMonth + 12U); /* 把1月/2月换算成13月/14月。 */
        u16CalculationYear--;                                       /* 年份减1，配合公式计算。 */
    }

    u16YearLowPart = (uint16_t)(u16CalculationYear % 100U); /* 取年份后两位。 */
    u16YearHighPart = (uint16_t)(u16CalculationYear / 100U); /* 取年份前两位。 */
    u16WeekFormulaValue = (uint16_t)((day + ((13U * (u8CalculationMonth + 1U)) / 5U) + u16YearLowPart + (u16YearLowPart / 4U) + (u16YearHighPart / 4U) + (5U * u16YearHighPart)) % 7U); /* 蔡勒公式。 */

    return (uint8_t)(((u16WeekFormulaValue + 5U) % 7U) + 1U); /* 转成1~7，其中1=周一，7=周日。 */
}

/* 检查上层传入的时间是否合法，非法时间不允许写入RTC。 */
static bool d_rtc_check_time(const d_rtc_time_t *time)
{
    uint8_t u8MaximumDay; /* 当前年月允许的最大日期。 */

    if (time == 0) /* 空指针不能访问。 */
    {
        return false; /* 返回参数错误。 */
    }

    if ((time->year < 2000U) || (time->year > 2099U)) /* RTC年份寄存器只有0~99，这里规定为2000~2099。 */
    {
        return false;                                 /* 年份超范围。 */
    }

    if ((time->month < 1U) || (time->month > 12U)) /* 月份必须是1~12。 */
    {
        return false;                              /* 月份超范围。 */
    }

    u8MaximumDay = d_rtc_get_month_days(time->year, time->month); /* 按年月计算当月最大日期。 */
    if ((time->day < 1U) || (time->day > u8MaximumDay))            /* 日期必须在当月范围内。 */
    {
        return false;                                       /* 日期超范围。 */
    }

    if (time->week > 7U) /* week=0表示自动计算，1~7表示周一~周日。 */
    {
        return false;    /* 星期超范围。 */
    }

    if ((time->hour > 23U) || (time->min > 59U) || (time->sec > 59U)) /* 检查24小时制时分秒范围。 */
    {
        return false;                                                 /* 时分秒超范围。 */
    }

    return true; /* 所有字段都合法。 */
}

/* 打开备份域访问权限，RTC和备份寄存器都属于备份域。 */
static void d_rtc_enable_backup_access(void)
{
    RCC_EnableAPB1PeriphClk((RCC_APB1_PERIPH_PWR | RCC_APB1_PERIPH_BKP), ENABLE); /* 打开PWR和BKP外设时钟。 */
    PWR_BackupAccessEnable(ENABLE);                                               /* 允许软件写RTC/备份域寄存器。 */
}

/* 从已选的其它RTC时钟源切换到LSE前，必须复位备份域。 */
static void d_rtc_prepare_clock_source(void)
{
    uint32_t rtc_sel = (RCC->BDCTRL & RCC_BDCTRL_RTCSEL);

    if ((rtc_sel != RCC_BDCTRL_RTCSEL_NOCLOCK) && (rtc_sel != RCC_BDCTRL_RTCSEL_LSE))
    {
        RCC_EnableBackupReset(ENABLE);  /* 旧时钟源及旧日历/校时标记一并清除。 */
        RCC_EnableBackupReset(DISABLE);
    }
}

/* 已起振的LSE保持运行，避免每次复位MCU时停振影响RTC走时。 */
static bool d_rtc_enable_lse_clock(void)
{
    uint32_t start_ms;

    if (RCC_GetFlagStatus(RCC_FLAG_LSERD) != RESET)
    {
        return true;
    }

    RCC_ConfigLse(RCC_LSE_ENABLE);
    start_ms = d_timer_get_ms();
    while (RCC_GetFlagStatus(RCC_FLAG_LSERD) == RESET)
    {
        if ((uint32_t)(d_timer_get_ms() - start_ms) >= D_RTC_LSE_READY_TIMEOUT_MS)
        {
            return false;
        }
    }

    return true;
}

/* 晶振就绪后选择LSE作为RTC时钟源。 */
static bool d_rtc_select_clock_source(void)
{
    uint32_t rtc_sel = (RCC->BDCTRL & RCC_BDCTRL_RTCSEL); /* 读取当前RTC时钟源选择位。 */

    if (rtc_sel == RCC_BDCTRL_RTCSEL_LSE) /* 已经是LSE时不重复配置。 */
    {
        return true;                      /* 当前时钟源正确。 */
    }

    RCC_ConfigRtcClk(RCC_RTCCLK_SRC_LSE); /* 把RTC时钟源切到外部LSE。 */
    return ((RCC->BDCTRL & RCC_BDCTRL_RTCSEL) == RCC_BDCTRL_RTCSEL_LSE); /* 重新读取确认是否切换成功。 */
}

/* 配置RTC日历分频和24小时制，分频值只影响走时速度，不会主动写日期和时间。 */
static bool d_rtc_config_calendar(void)
{
    RTC_InitType rtc_init; /* 国民库RTC初始化结构体。 */

    RTC_StructInit(&rtc_init);                           /* 先填入库默认值，防止结构体字段未初始化。 */
    rtc_init.RTC_HourFormat = RTC_24HOUR_FORMAT;         /* 使用24小时制。 */
    rtc_init.RTC_AsynchPrediv = D_RTC_LSE_ASYNC_PREDIV;  /* 32.768kHz先除以128。 */
    rtc_init.RTC_SynchPrediv = D_RTC_LSE_SYNC_PREDIV;    /* 再除以256，得到1Hz。 */

    if (RTC_Init(&rtc_init) != SUCCESS) /* 写入RTC分频和小时制配置。 */
    {
        return false;                  /* 初始化模式进入失败或写配置失败。 */
    }

    return (RTC_WaitForSynchro() == SUCCESS); /* 等待RTC影子寄存器同步完成。 */
}

/* 初始化RTC底层硬件；成功后只表示RTC能走时，不代表时间已经被校准过。 */
bool d_rtc_init(void)
{
    s_d_rtc_init_failed = 0U;        /* 主动初始化时先清掉失败标记，允许重新尝试。 */
    d_rtc_enable_backup_access();    /* 先打开备份域访问权限。 */
    d_rtc_prepare_clock_source();    /* 旧RTC源不是LSE时复位备份域。 */

    if (!d_rtc_enable_lse_clock())   /* 启动LSE并等待就绪。 */
    {
        s_d_rtc_inited = 0U;         /* 标记RTC本次没有初始化成功。 */
        s_d_rtc_init_failed = 1U;    /* 记录失败，普通读接口不再反复等待超时。 */
        return false;                /* 返回初始化失败。 */
    }

    if (!d_rtc_select_clock_source()) /* 确认RTC时钟源已经切到LSE。 */
    {
        s_d_rtc_inited = 0U;          /* 标记RTC本次没有初始化成功。 */
        s_d_rtc_init_failed = 1U;     /* 记录失败。 */
        return false;                 /* 返回初始化失败。 */
    }

    RCC_EnableRtcClk(ENABLE);         /* 打开RTC外设时钟。 */
    if (!d_rtc_config_calendar())      /* 配置RTC日历分频和24小时制。 */
    {
        s_d_rtc_inited = 0U;           /* 标记RTC本次没有初始化成功。 */
        s_d_rtc_init_failed = 1U;      /* 记录失败。 */
        return false;                  /* 返回初始化失败。 */
    }

    s_d_rtc_inited = 1U;               /* 标记RTC硬件初始化成功。 */
    return true;                       /* 返回初始化成功。 */
}

/* 写入RTC真实时间；写成功后会保存备份标记，后续d_rtc_read()才会认为时间有效。 */
bool d_rtc_write(const d_rtc_time_t *time)
{
    RTC_DateType rtc_date; /* 国民库日期结构体，写入RTC_DATE寄存器。 */
    RTC_TimeType rtc_time; /* 国民库时间结构体，写入RTC_TSH寄存器。 */

    if (!d_rtc_check_time(time)) /* 先检查上层传入的时间是否合法。 */
    {
        return false;           /* 时间参数非法，不写RTC。 */
    }

    if ((s_d_rtc_inited == 0U) && (!d_rtc_init())) /* 如果RTC还没初始化，写时间前主动初始化一次。 */
    {
        return false;                           /* 初始化失败，不能写RTC。 */
    }

    rtc_date.Year = (uint8_t)(time->year - 2000U); /* RTC年份寄存器保存0~99，这里转成2000年偏移。 */
    rtc_date.Month = time->month;                  /* 写入月份。 */
    rtc_date.Date = time->day;                     /* 写入日期。 */
    rtc_date.WeekDay = (time->week == 0U) ? d_rtc_calc_week(time->year, time->month, time->day) : time->week; /* week=0时自动计算。 */

    rtc_time.Hours = time->hour;     /* 写入小时。 */
    rtc_time.Minutes = time->min;    /* 写入分钟。 */
    rtc_time.Seconds = time->sec;    /* 写入秒。 */
    rtc_time.H12 = RTC_AM_H12;       /* 24小时制下该字段固定为AM，库函数会忽略12小时制含义。 */

    if (RTC_SetDate(RTC_FORMAT_BIN, &rtc_date) != SUCCESS) /* 按二进制格式写日期，库内部会转BCD。 */
    {
        return false;                                     /* 日期写失败。 */
    }

    if (RTC_ConfigTime(RTC_FORMAT_BIN, &rtc_time) != SUCCESS) /* 按二进制格式写时间，库内部会转BCD。 */
    {
        return false;                                       /* 时间写失败。 */
    }

    BKP_WriteBkpData(D_RTC_BKP_MARK_REG, D_RTC_BKP_MARK_VALUE); /* 保存“RTC时间已设置”的备份标记。 */
    return true;                                               /* 日期、时间、标记都写成功。 */
}

/* 读取RTC真实时间；如果还没有写入过有效时间，返回false。 */
bool d_rtc_read(d_rtc_time_t *time)
{
    if (time == 0)         /* 空指针不能写输出结果。 */
    {
        return false;      /* 返回参数错误。 */
    }

    if (s_d_rtc_inited == 0U)           /* 如果RTC还没有完成初始化。 */
    {
        if (s_d_rtc_init_failed != 0U)  /* 普通读取曾经初始化失败过。 */
        {
            return false;               /* 直接返回失败，避免SaveLog_Task反复等待LSE超时。 */
        }

        if (!d_rtc_init())              /* 尝试自动初始化RTC。 */
        {
            return false;               /* 自动初始化失败。 */
        }
    }

    if (!d_rtc_is_valid())              /* 检查备份寄存器里是否有“时间已设置”标记。 */
    {
        return false;                   /* 没有写过有效时间，读出的日历不可信。 */
    }

    return d_rtc_read_calendar(time); /* 已校时接口沿用相同的日历读取和字段校验。 */
}

/* 读取RTC当前日历；备份标志只记录是否校过时，不作为日历读取门槛。 */
bool d_rtc_read_calendar(d_rtc_time_t *time)
{
    RTC_DateType rtc_date; /* 保存从RTC_DATE寄存器读出的日期。 */
    RTC_TimeType rtc_time; /* 保存从RTC_TSH寄存器读出的时间。 */

    if (time == 0) /* 空指针不能写输出结果。 */
    {
        return false; /* 返回参数错误。 */
    }

    if (s_d_rtc_inited == 0U) /* 如果RTC尚未完成硬件初始化。 */
    {
        if (s_d_rtc_init_failed != 0U) /* 本次上电初始化已经失败过。 */
        {
            return false; /* 等待上层显式重试初始化。 */
        }

        if (!d_rtc_init()) /* 首次读取时尝试初始化RTC。 */
        {
            return false; /* 硬件初始化失败。 */
        }
    }

    RTC_GetTime(RTC_FORMAT_BIN, &rtc_time); /* 先读时间，符合RTC库读取时序。 */
    RTC_GetDate(RTC_FORMAT_BIN, &rtc_date); /* 再读日期，锁定同一次日历快照。 */

    time->year = (uint16_t)(2000U + rtc_date.Year); /* RTC年份偏移转回工程使用的2000~2099。 */
    time->month = rtc_date.Month;                   /* 输出月份。 */
    time->day = rtc_date.Date;                      /* 输出日期。 */
    time->week = rtc_date.WeekDay;                  /* 输出星期。 */
    time->hour = rtc_time.Hours;                    /* 输出小时。 */
    time->min = rtc_time.Minutes;                   /* 输出分钟。 */
    time->sec = rtc_time.Seconds;                   /* 输出秒。 */

    return d_rtc_check_time(time); /* 字段合法即可使用，不要求备份标志已置位。 */
}

/* 读取RTC日内秒数；相对计时只依赖RTC持续走时，不依赖用户校准日历日期。 */
bool d_rtc_read_seconds_of_day(uint32_t *seconds)
{
    RTC_TimeType rtc_time;

    if (seconds == 0)
    {
        return false;
    }

    if ((s_d_rtc_inited == 0U) && !d_rtc_init())
    {
        return false;
    }

    RTC_GetTime(RTC_FORMAT_BIN, &rtc_time);
    if ((rtc_time.Hours > 23U) ||
        (rtc_time.Minutes > 59U) ||
        (rtc_time.Seconds > 59U))
    {
        return false;
    }

    *seconds = ((uint32_t)rtc_time.Hours * 3600UL) +
               ((uint32_t)rtc_time.Minutes * 60UL) +
               (uint32_t)rtc_time.Seconds;
    return true;
}

/* 将RTC日历转换为2000-01-01起的秒数，避免跨日时把手工回拨误判为午夜。 */
bool d_rtc_read_seconds_since_2000(uint32_t *seconds)
{
    d_rtc_time_t time;
    uint32_t days = 0UL;
    uint16_t year;
    uint8_t month;

    if ((seconds == 0) || !d_rtc_read_calendar(&time))
    {
        return false;
    }

    for (year = 2000U; year < time.year; year++)
    {
        days += d_rtc_is_leap_year(year) ? 366UL : 365UL;
    }
    for (month = 1U; month < time.month; month++)
    {
        days += d_rtc_get_month_days(time.year, month);
    }
    days += (uint32_t)(time.day - 1U);

    *seconds = (days * 86400UL) +
               ((uint32_t)time.hour * 3600UL) +
               ((uint32_t)time.min * 60UL) +
               (uint32_t)time.sec;
    return true;
}

/* 判断RTC时间是否已经被上层写入过；初始化成功但没写时间时仍然返回false。 */
bool d_rtc_is_valid(void)
{
    if (s_d_rtc_inited == 0U) /* RTC硬件都没初始化成功时，时间一定无效。 */
    {
        return false;        /* 返回无效。 */
    }

    return (BKP_ReadBkpData(D_RTC_BKP_MARK_REG) == D_RTC_BKP_MARK_VALUE); /* 备份标记匹配才认为时间有效。 */
}
static volatile uint8_t s_d_rtc_wakeup_pending; /* RTC自动唤醒中断置位，主循环醒来后读取并清除。 */

/* 配置RTC WakeUp使用的EXTI20和RTC_IRQn，STOP0下RTC事件通过这一路唤醒CPU。 */
static void d_rtc_wakeup_exti_config(FunctionalState cmd)
{
    EXTI_InitType exti_init;
    NVIC_InitType nvic_init;

    EXTI_ClrITPendBit(EXTI_LINE20);
    EXTI_InitStruct(&exti_init);
    exti_init.EXTI_Line = EXTI_LINE20;
    exti_init.EXTI_Mode = EXTI_Mode_Interrupt;
    exti_init.EXTI_Trigger = EXTI_Trigger_Rising;
    exti_init.EXTI_LineCmd = cmd;
    EXTI_InitPeripheral(&exti_init);

    nvic_init.NVIC_IRQChannel = RTC_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 1;
    nvic_init.NVIC_IRQChannelSubPriority = 0;
    nvic_init.NVIC_IRQChannelCmd = cmd;
    NVIC_Init(&nvic_init);
}

bool d_rtc_wakeup_start(uint16_t seconds)
{
    if (seconds == 0U)
    {
        return false;
    }

    if ((s_d_rtc_inited == 0U) && (!d_rtc_init()))
    {
        return false;
    }

    s_d_rtc_wakeup_pending = 0U;

    /* 厂家要求修改唤醒时钟和计数前先关闭WUT，并等待WTWF允许写入。 */
    if (RTC_EnableWakeUp(DISABLE) != SUCCESS)
    {
        return false;
    }

    RTC_ClrFlag(RTC_FLAG_WTF);
    RTC_ClrIntPendingBit(RTC_INT_WUT);
    EXTI_ClrITPendBit(EXTI_LINE20);
    NVIC_ClearPendingIRQ(RTC_IRQn);

    RTC_ConfigWakeUpClock(RTC_WKUPCLK_CK_SPRE_16BITS);
    /* 芯片手册规定周期为(WKUPT + 1)个ck_wut，因此目标秒数要减1写入。 */
    RTC_SetWakeUpCounter((uint32_t)(seconds - 1U));

    /* 读回计数器，避免配置未生效时仍然进入STOP且失去预期唤醒时间。 */
    if (RTC_GetWakeUpCounter() != (uint32_t)(seconds - 1U))
    {
        return false;
    }

    RTC_ConfigInt(RTC_INT_WUT, ENABLE);
    d_rtc_wakeup_exti_config(ENABLE);

    return (RTC_EnableWakeUp(ENABLE) == SUCCESS);
}

void d_rtc_wakeup_stop(void)
{
    RTC_EnableWakeUp(DISABLE);
    RTC_ConfigInt(RTC_INT_WUT, DISABLE);
    RTC_ClrFlag(RTC_FLAG_WTF);
    RTC_ClrIntPendingBit(RTC_INT_WUT);
    EXTI_ClrITPendBit(EXTI_LINE20);
    d_rtc_wakeup_exti_config(DISABLE);
    NVIC_ClearPendingIRQ(RTC_IRQn);
    s_d_rtc_wakeup_pending = 0U;
}

bool d_rtc_wakeup_is_pending(void)
{
    return (s_d_rtc_wakeup_pending != 0U);
}

void d_rtc_wakeup_clear_pending(void)
{
    s_d_rtc_wakeup_pending = 0U;
}

void d_rtc_wakeup_irq_handler(void)
{
    FlagStatus eWakeupTimerFlag = RTC_GetFlagStatus(RTC_FLAG_WTF);
    FlagStatus eWakeupInterrupt = RTC_GetITStatus(RTC_INT_WUT);
    FlagStatus eExtiPending = EXTI_GetStatusFlag(EXTI_LINE20);

    /* 只有RTC WUTF证明定时器确实到期；单独的EXTI/NVIC pending不能算RTC唤醒。 */
    if (eWakeupTimerFlag != RESET)
    {
        s_d_rtc_wakeup_pending = 1U;
    }

    /* 清理唤醒源及其外部中断挂起，避免陈旧状态重复触发。 */
    if ((eWakeupTimerFlag != RESET) ||
        (eWakeupInterrupt != RESET) ||
        (eExtiPending != RESET))
    {
        RTC_ClrFlag(RTC_FLAG_WTF);
        RTC_ClrIntPendingBit(RTC_INT_WUT);
        EXTI_ClrITPendBit(EXTI_LINE20);
    }
}

