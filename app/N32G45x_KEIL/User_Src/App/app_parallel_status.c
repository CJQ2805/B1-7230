/*
 * 并联电池包状态缓存。
 * 本模块接收并解析其它BMS包已有的B1主动报文，并把本机快照放入同一张地址表；
 * 不负责发送并联报文，也不负责控制充/放电MOS。
 */
#include "App/app_parallel_status.h"
#include "App/app_parallel_id.h"

#include <string.h>

#include "App/app_bms_snapshot.h"
#include "Driver/d_timer.h"
#include "FreeRTOS.h"
#include "Protocol/Comm/zs_b1_can_protocol.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "task.h"

/*
 * 本机与对端共用一组连续地址槽位：
 * slot 0对应DUCAR_CAN_BMS_A_ADDRESS，之后slot递增时CAN源地址也加1。
 * 可用槽位数量由编址模块的APP_ID_ASSIGN_MAX_PACK_COUNT统一限定。
 */
#define APP_PARALLEL_STATUS_PGN_BASE       ((uint16_t)0xF20DU)
#define APP_PARALLEL_STATUS_PGN_LAST       ((uint16_t)0xF221U)
/* 本机数据每100ms从业务快照刷新一次；对端5秒没有主动报文则离线清空。 */
#define APP_PARALLEL_STATUS_LOCAL_REFRESH_MS ((uint32_t)100UL)
/* 0xFF不可能是有效的BMS槽位序号，因此用作地址/PGN查找失败的返回值。 */
#define APP_PARALLEL_STATUS_INVALID_SLOT   ((uint8_t)0xFFU)
/*
 * 未采样数据的缓存标记：U16沿用协议常见无效值0xFFFF；S16/S8用于温度缓存字段。
 * 读缓存字段的调用方应先看valid_pgn_mask、在线状态以及相应传感器有效状态。
 */
#define APP_PARALLEL_STATUS_INVALID_U16    ((uint16_t)0xFFFFU)
#define APP_PARALLEL_STATUS_INVALID_S16    ((int16_t)-400)
#define APP_PARALLEL_STATUS_INVALID_S8     ((int8_t)-128)

/* 本机与对端BMS都按CAN源地址保存最近状态；数组下标与连续地址一一对应。 */
static app_parallel_status_pack_t s_parallel_status[APP_ID_ASSIGN_MAX_PACK_COUNT];
/* 当前本机地址映射到的槽位，未映射成功时为INVALID_SLOT。 */
static uint8_t s_local_slot = APP_PARALLEL_STATUS_INVALID_SLOT;
/* 最近一次刷新本机槽位的TIM6毫秒时间戳。 */
static uint32_t s_local_update_ms;
/* 记录缓存当前是否曾在成品模式下启用，以便离开成品模式时只清表一次。 */
static uint8_t s_parallel_status_active;
/* 真正静默期间冻结缓存年龄；唤醒后整体平移其时间戳，避免误判成员离线。 */
static volatile uint8_t s_silent_hold_active;
static uint32_t s_silent_start_ms;
/* 唤醒后立即刷新一次本机快照，而不是等待常规100ms周期。 */
static uint8_t s_local_refresh_pending;

/* 并联状态缓存与编址、MOS控制共用同一个总开关。 */
static uint8_t app_parallel_status_is_enabled(void)
{
    return app_parallel_id_is_enabled();
}
/*
 * 将CAN数据中的Data[offset]和Data[offset+1]按小端序组成U16。
 * 参数：data为8字节报文数据；offset为低字节位置。
 * 返回：原始无符号数值，不做协议比例/偏移换算。
 */
static uint16_t app_parallel_status_read_u16_le(const uint8_t *data,
                                                uint8_t offset)
{
    return (uint16_t)((uint16_t)data[offset] |
                      ((uint16_t)data[(uint8_t)(offset + 1U)] << 8U));
}

/*
 * 读取小端有符号16位二进制补码值。
 * 先按U16读取，再依据符号位做显式补码还原，避免依赖无符号到有符号窄化的实现细节。
 * 返回的仍是原始整数计数，摄氏度、电流等单位由对应PGN字段定义。
 */
static int16_t app_parallel_status_read_s16_le(const uint8_t *data,
                                               uint8_t offset)
{
    uint16_t value = app_parallel_status_read_u16_le(data, offset);

    if ((value & 0x8000U) == 0U)
    {
        return (int16_t)value;
    }

    return (int16_t)(-1 - (int32_t)(0xFFFFU - value));
}

/*
 * 将单字节二进制补码解释为有符号温度整数。
 * 该转换只改变符号解释，不做0.1℃到℃的单位换算；调用位置负责按协议字段处理单位。
 */
static int8_t app_parallel_status_read_s8(uint8_t value)
{
    if ((value & 0x80U) == 0U)
    {
        return (int8_t)value;
    }

    return (int8_t)(-1 - (int16_t)(0xFFU - value));
}









/*
 * 将内部S16温度压入协议S8可用范围[-127,127]。
 * -128保留给APP_PARALLEL_STATUS_INVALID_S8，因此有效温度不允许落到该编码。
 */
static int8_t app_parallel_status_s8_from_s16(int16_t value)
{
    if (value > 127)
    {
        return 127;
    }
    if (value < -127)
    {
        return (int8_t)-127;
    }
    return (int8_t)value;
}

/*
 * 将快照温度数组下标转换为业务层NTC故障位：0=AFE NTC、1=电芯NTC1、2=电芯NTC2。
 * 其它下标当前没有独立故障位，返回0；调用方因此只依据temp_valid_mask判断有效性。
 */
static uint8_t app_parallel_status_temperature_fault_bit(uint8_t index)
{
    switch (index)
    {
        case 0U:
            return APP_BMS_NTC_FAULT_AFE;
        case 1U:
            return APP_BMS_NTC_FAULT_CELL1;
        case 2U:
            return APP_BMS_NTC_FAULT_CELL2;
        default:
            return 0U;
    }
}

/*
 * 将U32限制到U16字段范围；超过0xFFFF时饱和到0xFFFF，而不是截低16位回绕。
 * 常用于mV、容量和SOP限值从较宽业务结构写入缓存字段时。
 */
static uint16_t app_parallel_status_u16_from_u32(uint32_t value)
{
    return (value > 0xFFFFUL) ? 0xFFFFU : (uint16_t)value;
}

/*
 * 将S32限制到S16可表示范围；正负越界分别饱和到INT16_MAX/INT16_MIN。
 * 业务电流保留正负方向，因此不能使用无符号转换函数。
 */
static int16_t app_parallel_status_s16_from_s32(int32_t value)
{
    if (value > 32767L)
    {
        return 32767;
    }
    if (value < -32768L)
    {
        return (int16_t)-32768;
    }
    return (int16_t)value;
}

/*
 * 把支持的PGN转换为valid_pgn_mask位序号。
 * 返回0..20表示PGN属于F20D~F221；超出范围返回INVALID_SLOT。
 * 当前B1主动PGN区间连续，所以偏移可直接作为掩码位号。
 */
static uint8_t app_parallel_status_pgn_index(uint16_t pgn)
{
    if ((pgn >= APP_PARALLEL_STATUS_PGN_BASE) &&
        (pgn <= APP_PARALLEL_STATUS_PGN_LAST))
    {
        /* B1当前已定义的F20D~F221主动PGN连续，可直接映射有效位序号。 */
        return (uint8_t)(pgn - APP_PARALLEL_STATUS_PGN_BASE);
    }
    return APP_PARALLEL_STATUS_INVALID_SLOT;
}

/*
 * 将B1报文源地址转换为状态表槽位。
 * 地址低于基地址或偏移超出槽位数均拒绝；地址按连续区间映射，不在中间跳过地址。
 */
static uint8_t app_parallel_status_slot_from_address(uint8_t address)
{
    uint8_t slot;

    if (address < DUCAR_CAN_BMS_A_ADDRESS)
    {
        return APP_PARALLEL_STATUS_INVALID_SLOT;
    }

    slot = (uint8_t)(address - DUCAR_CAN_BMS_A_ADDRESS);
    if (slot >= APP_ID_ASSIGN_MAX_PACK_COUNT)
    {
        return APP_PARALLEL_STATUS_INVALID_SLOT;
    }
    return slot;
}

/*
 * 清空指定槽位，供初始化、地址切换、超时下线和过期后重上线共用。
 * memset先将online和valid_pgn_mask清零；随后恢复该槽位应有的source_address，
 * 并将电压/温度字段设为无效标记。其它状态归零，表示当前没有可用的旧报文状态。
 * slot越界时不操作任何内存。
 */
static void app_parallel_status_clear_pack(app_parallel_status_pack_t *pack,
                                          uint8_t slot)
{
    uint8_t index;

    if ((pack != 0) && (slot < APP_ID_ASSIGN_MAX_PACK_COUNT))
    {
        memset(pack, 0, sizeof(*pack));
        pack->source_address = (uint8_t)(DUCAR_CAN_BMS_A_ADDRESS + slot);
        pack->max_cell_mv = APP_PARALLEL_STATUS_INVALID_U16;
        pack->min_cell_mv = APP_PARALLEL_STATUS_INVALID_U16;
        pack->mos_temp_c = APP_PARALLEL_STATUS_INVALID_S8;
        pack->ptc_temp_c = APP_PARALLEL_STATUS_INVALID_S8;
        pack->max_cell_temp_0p1c = APP_PARALLEL_STATUS_INVALID_S16;
        pack->min_cell_temp_0p1c = APP_PARALLEL_STATUS_INVALID_S16;
        for (index = 0U; index < APP_BMS_MAX_CELL_NUM; ++index)
        {
            pack->cell_mv[index] = APP_PARALLEL_STATUS_INVALID_U16;
        }
        for (index = 0U; index < 4U; ++index)
        {
            pack->cell_temp_0p1c[index] = APP_PARALLEL_STATUS_INVALID_S16;
        }
    }
}

/* 单槽清理与CAN接收并发，因此用短临界区保护整槽清零。 */
static void app_parallel_status_clear_slot(uint8_t slot)
{
    if (slot < APP_ID_ASSIGN_MAX_PACK_COUNT)
    {
        taskENTER_CRITICAL();
        app_parallel_status_clear_pack(&s_parallel_status[slot], slot);
        taskEXIT_CRITICAL();
    }
}





/*
 * 快照SOP电流按0.1A计数，缓存字段按0.01A计数，因此正值乘10。
 * 允许电流不能为负数；0和负值归一为0，超过U16范围时饱和。
 */
static uint16_t app_parallel_status_sop_current_001a(int16_t current_0p1a)
{
    if (current_0p1a <= 0)
    {
        return 0U;
    }
    return app_parallel_status_u16_from_u32((uint32_t)current_0p1a * 10UL);
}

/*
 * 从应用层一致性快照构造本机状态槽位。
 * 地址变化时先清理旧本机槽位，再刷新当前地址；本函数只写RAM缓存，不发送报文或控制MOS。
 */
static void app_parallel_status_update_local(uint32_t now_ms)
{
    app_bms_snapshot_t snapshot;
    app_parallel_status_pack_t local_pack;
    app_parallel_status_pack_t *pack;
    uint32_t total_voltage_001v;
    int32_t current_001a;
    int32_t index;
    uint8_t temperature_index;
    uint8_t temperature_fault_bit;
    uint8_t valid_temperature_count;
    int16_t temperature_0p1c;
    uint8_t slot;

    /* 全局地址尚未落入允许的连续区间时，不更新数组。 */
    slot = app_parallel_status_slot_from_address(g_ducar_can_bms_address);
    if (slot == APP_PARALLEL_STATUS_INVALID_SLOT)
    {
        return;
    }

    if ((s_local_slot != APP_PARALLEL_STATUS_INVALID_SLOT) &&
        (s_local_slot != slot))
    {
        /* 本机rank变化后清掉原槽位，避免旧本机数据被算作在线对端。 */
        app_parallel_status_clear_slot(s_local_slot);
    }

    /* 一次读取锁保护的快照，防止各字段来自不同更新时刻。 */
    app_bms_snapshot_read(&snapshot);
    /* 先在栈上构造完整本机槽位，再用短临界区一次性提交，避免CAN任务读到半更新数据。 */
    pack = &local_pack;
    app_parallel_status_clear_pack(pack, slot);

    pack->online = 1U;
    pack->source_address = g_ducar_can_bms_address;
    pack->last_update_ms = now_ms;
    /* 本机可由业务快照提供F20D~F219对应的数据，因此置位这13个有效组标志。 */
    pack->valid_pgn_mask = APP_PARALLEL_STATUS_PGN_BIT_F20D |
                           APP_PARALLEL_STATUS_PGN_BIT_F20E |
                           APP_PARALLEL_STATUS_PGN_BIT_F20F |
                           APP_PARALLEL_STATUS_PGN_BIT_F210 |
                           APP_PARALLEL_STATUS_PGN_BIT_F211 |
                           APP_PARALLEL_STATUS_PGN_BIT_F212 |
                           APP_PARALLEL_STATUS_PGN_BIT_F213 |
                           APP_PARALLEL_STATUS_PGN_BIT_F214 |
                           APP_PARALLEL_STATUS_PGN_BIT_F215 |
                           APP_PARALLEL_STATUS_PGN_BIT_F216 |
                           APP_PARALLEL_STATUS_PGN_BIT_F217 |
                           APP_PARALLEL_STATUS_PGN_BIT_F218 |
                           APP_PARALLEL_STATUS_PGN_BIT_F219;
    for (index = 0; index <= 12; ++index)
    {
        pack->pgn_last_update_ms[index] = now_ms;
    }

    /* F20D换算：mV/10得0.01V，mA/10得0.01A，SOC整数百分比乘10得0.1%。 */
    total_voltage_001v = snapshot.bat_mv / 10UL;
    pack->total_voltage_001v = app_parallel_status_u16_from_u32(total_voltage_001v);
    current_001a = snapshot.current_business_ma / 10L;
    pack->current_001a = app_parallel_status_s16_from_s32(current_001a);
    pack->soc_0p1_percent = (uint16_t)snapshot.soc.u8display_soc * 10U;
    pack->soh_percent = snapshot.soc.u8display_soh;
    pack->detc_status = (snapshot.signal.input.mcu_detc != 0U) ? 1U : 2U;

    /* F20E的SOP限流来自快照，内部0.1A计数转成缓存0.01A计数。 */
    pack->max_discharge_current_001a =
        app_parallel_status_sop_current_001a(snapshot.sop.s16MaxDischargeCurrent10mA);
    pack->max_charge_current_001a =
        app_parallel_status_sop_current_001a(snapshot.sop.s16MaxChargeCurrent10mA);

    /* 未ready时单体极值无效；数组只复制配置串数与有效串数的交集。 */
    pack->max_cell_mv = (snapshot.data_ready != 0U) ?
                        snapshot.max_cell_mv : APP_PARALLEL_STATUS_INVALID_U16;
    pack->min_cell_mv = (snapshot.data_ready != 0U) ?
                        snapshot.min_cell_mv : APP_PARALLEL_STATUS_INVALID_U16;
    for (index = 0; index < (int32_t)APP_BMS_MAX_CELL_NUM; ++index)
    {
        pack->cell_mv[index] = APP_PARALLEL_STATUS_INVALID_U16;
        if ((index < (int32_t)APP_BMS_CELL_COUNT) &&
            (index < (int32_t)snapshot.cell_count))
        {
            /* 快照中的单体数组已使用逻辑索引，AFE物理映射在快照层完成。 */
            pack->cell_mv[index] = snapshot.cell_mv[index];
        }
    }

    /* NTC默认写无效标记；仅复制有效且没有对应温度故障的通道。 */
    for (index = 0; index < 4L; ++index)
    {
        pack->cell_temp_0p1c[index] = APP_PARALLEL_STATUS_INVALID_S16;
        if (index < (int32_t)APP_BMS_MAX_TEMP_NUM)
        {
            temperature_fault_bit =
                app_parallel_status_temperature_fault_bit((uint8_t)index);
            if (((snapshot.temp_valid_mask & (uint8_t)(1U << index)) != 0U) &&
                ((temperature_fault_bit == 0U) ||
                 ((snapshot.ntc.fault_mask & temperature_fault_bit) == 0U)))
            {
                pack->cell_temp_0p1c[index] = snapshot.temp_0p1c[index];
            }
        }
    }

    /* MOS/PTC内部快照为0.1℃，协议字段为整数℃；无效时写-128哨兵值。 */
    pack->mos_temp_c = ((snapshot.ntc.valid_mask & APP_BMS_NTC_VALID_MOS) != 0U) ?
                       app_parallel_status_s8_from_s16(
                           (int16_t)(snapshot.ntc.mos_temp_0p1c / 10)) :
                       APP_PARALLEL_STATUS_INVALID_S8;
    pack->ptc_temp_c = ((snapshot.ntc.valid_mask & APP_BMS_NTC_VALID_PTC) != 0U) ?
                       app_parallel_status_s8_from_s16(
                           (int16_t)(snapshot.ntc.ptc_temp_0p1c / 10)) :
                       APP_PARALLEL_STATUS_INVALID_S8;

    /* 从有效且无故障的电芯NTC计算极值；没有有效通道时保持无效哨兵值。 */
    pack->max_cell_temp_0p1c = APP_PARALLEL_STATUS_INVALID_S16;
    pack->min_cell_temp_0p1c = APP_PARALLEL_STATUS_INVALID_S16;
    valid_temperature_count = 0U;
    for (temperature_index = 0U;
         temperature_index < APP_BMS_MAX_TEMP_NUM;
         ++temperature_index)
    {
        if ((snapshot.temp_valid_mask &
             (uint8_t)(1U << temperature_index)) == 0U)
        {
            continue;
        }
        temperature_fault_bit =
            app_parallel_status_temperature_fault_bit(temperature_index);
        if ((temperature_fault_bit != 0U) &&
            ((snapshot.ntc.fault_mask & temperature_fault_bit) != 0U))
        {
            continue;
        }

        temperature_0p1c = snapshot.temp_0p1c[temperature_index];
        if (valid_temperature_count == 0U)
        {
            pack->max_cell_temp_0p1c = temperature_0p1c;
            pack->min_cell_temp_0p1c = temperature_0p1c;
        }
        else
        {
            if (temperature_0p1c > pack->max_cell_temp_0p1c)
            {
                pack->max_cell_temp_0p1c = temperature_0p1c;
            }
            if (temperature_0p1c < pack->min_cell_temp_0p1c)
            {
                pack->min_cell_temp_0p1c = temperature_0p1c;
            }
        }
        ++valid_temperature_count;
    }
    pack->cell_count = snapshot.cell_count;
    pack->ntc_count = (uint8_t)APP_BMS_MAX_TEMP_NUM;

    /* 容量内部单位mAh，缓存协议单位0.01Ah时除10；半充次数暂无业务来源，填默认0。 */
    pack->cycle_count = app_parallel_status_u16_from_u32(snapshot.soc.u32cycle_index);
    pack->fcc_001ah = (snapshot.soc.s32battery_full_cap > 0) ?
                      app_parallel_status_u16_from_u32((uint32_t)snapshot.soc.s32battery_full_cap / 10UL) : 0U;
    pack->remaining_capacity_001ah = (snapshot.soc.s32remain_cap > 0) ?
                                    app_parallel_status_u16_from_u32((uint32_t)snapshot.soc.s32remain_cap / 10UL) : 0U;
    pack->half_charge_count = 0U;

    /* 缓存本机插接、MOS、KEY和工作状态；仅记录反馈，不做MOS控制决策。 */
    pack->detd_present = (snapshot.signal.input.mcu_detd != 0U) ? 1U : 0U;
    pack->detc_present = (snapshot.signal.input.mcu_detc != 0U) ? 1U : 0U;
    pack->discharge_mos_on = (snapshot.mos.dsg_mos_state != 0U) ? 1U : 0U;
    pack->charge_mos_on = (snapshot.mos.chg_mos_state != 0U) ? 1U : 0U;
    pack->precharge_mos_on = (snapshot.mos.pre_mos_state != 0U) ? 1U : 0U;
    pack->key_present = (snapshot.signal.input.mcu_key_present != 0U) ? 1U : 0U;
    pack->parallel_mos_status[0] = 0x3FU;
    pack->parallel_mos_status[1] = 0x3FU;
    pack->work_status = snapshot.bms_work_status;

    /* F219 SOP限值：充电电压mV/10转0.01V，电流转为0.01A计数。 */
    pack->charge_voltage_001v =
        app_parallel_status_u16_from_u32(snapshot.sop.u32MaxChargeVoltageMv / 10UL);
    pack->charge_current_001a =
        app_parallel_status_sop_current_001a(snapshot.sop.s16MaxChargeCurrent10mA);
    pack->recharge_current_001a =
        app_parallel_status_u16_from_u32((uint32_t)snapshot.sop.u16MaxRechargeCurrent10mA * 10UL);
    pack->discharge_current_001a =
        app_parallel_status_sop_current_001a(snapshot.sop.s16MaxDischargeCurrent10mA);

    /* 本机故障掩码和AFE故障快照只复制，不在此处重新判定、改写或清除。 */
    pack->local_charge_fault_a = snapshot.fault.active.detail.ucharge_fault_A.u16fault_A;
    pack->local_charge_fault_b = snapshot.fault.active.detail.ucharge_fault_B.u16fault_B;
    pack->local_discharge_fault_a = snapshot.fault.active.detail.udischarge_fault_A.u16fault_A;
    pack->local_discharge_fault_b = snapshot.fault.active.detail.udischarge_fault_B.u16fault_B;
    pack->local_general_fault_a = snapshot.fault.active.detail.ugeneral_fault_A.u16fault_A;
    pack->local_afe_fault_valid = snapshot.afe_status_valid;
    pack->local_afe_cell_ov = snapshot.afe_fault.cell_ov;
    pack->local_afe_cell_uv = snapshot.afe_fault.cell_uv;
    pack->local_afe_charge_oc1 = snapshot.afe_fault.chg_oc1;
    pack->local_afe_charge_oc2 = snapshot.afe_fault.chg_oc2;
    pack->local_afe_discharge_oc1 = snapshot.afe_fault.dsg_oc1;
    pack->local_afe_discharge_oc2 = snapshot.afe_fault.dsg_oc2;
    pack->local_afe_short_circuit = snapshot.afe_fault.short_circuit;

    taskENTER_CRITICAL();
    s_parallel_status[slot] = local_pack;
    s_local_slot = slot;
    s_local_update_ms = now_ms;
    taskEXIT_CRITICAL();
}

void app_parallel_status_init(void)
{
    uint8_t slot;

    /*
     * 初始化本机和对端共用的全部地址槽位：先清零，再逐槽恢复固定源地址和无效字段标记。
     * 本机rank尚未建立，先将本机槽位标志设为INVALID；任务开始后再由业务快照填入对应槽位。
     * 初始化后所有槽位均offline且valid_pgn_mask为0，避免沿用上次CAN运行状态。
     */
    memset(s_parallel_status, 0, sizeof(s_parallel_status));
    s_local_slot = APP_PARALLEL_STATUS_INVALID_SLOT;
    s_local_update_ms = 0UL;
    s_parallel_status_active = 0U;
    s_silent_hold_active = 0U;
    s_silent_start_ms = 0UL;
    s_local_refresh_pending = 0U;
    for (slot = 0U; slot < APP_ID_ASSIGN_MAX_PACK_COUNT; ++slot)
    {
        app_parallel_status_clear_pack(&s_parallel_status[slot], slot);
    }
}

/* 唤醒后让缓存时钟跳过整段协议静默时间，再恢复常规新鲜度检查。 */
void app_parallel_status_resume_after_silence(void)
{
    uint32_t silent_duration_ms;
    uint32_t now_ms;
    uint8_t slot;
    uint8_t pgn_index;

    if (s_silent_hold_active == 0U)
    {
        taskENTER_CRITICAL();
        s_local_refresh_pending = 1U;
        taskEXIT_CRITICAL();
        return;
    }

    taskENTER_CRITICAL();
    if (s_silent_hold_active != 0U)
    {
        now_ms = d_timer_get_ms();
        silent_duration_ms = (uint32_t)(now_ms - s_silent_start_ms);
        for (slot = 0U; slot < APP_ID_ASSIGN_MAX_PACK_COUNT; ++slot)
        {
            if (s_parallel_status[slot].online == 0U)
            {
                continue;
            }

            s_parallel_status[slot].last_update_ms += silent_duration_ms;
            for (pgn_index = 0U; pgn_index < 21U; ++pgn_index)
            {
                if ((s_parallel_status[slot].valid_pgn_mask &
                     ((uint32_t)1UL << pgn_index)) != 0UL)
                {
                    s_parallel_status[slot].pgn_last_update_ms[pgn_index] += silent_duration_ms;
                }
            }
        }
        if (s_local_slot < APP_ID_ASSIGN_MAX_PACK_COUNT)
        {
            s_local_update_ms += silent_duration_ms;
        }
        s_silent_hold_active = 0U;
    }
    /* 即使静默状态刚建立、尚未被周期任务观察到，也请求唤醒后立即刷新本机槽位。 */
    s_local_refresh_pending = 1U;
    taskEXIT_CRITICAL();
}

void app_parallel_status_task(void)
{
    uint32_t now_ms; /* 本轮离线检查共用的TIM6毫秒时间戳。 */
    uint8_t slot;    /* 扫描全部地址槽位，并跳过当前本机槽位。 */
    uint8_t refresh_local;
    uint8_t silent_hold;

    /*
     * 生产/测试模式或并联宏关闭时不运行并联状态缓存。
     * 若刚从成品模式退出，只清空一次全部槽位，避免旧在线状态在下次启用时残留。
     */
    if (app_parallel_status_is_enabled() == 0U)
    {
        if (s_parallel_status_active != 0U)
        {
            for (slot = 0U; slot < APP_ID_ASSIGN_MAX_PACK_COUNT; ++slot)
            {
                app_parallel_status_clear_slot(slot);
            }
            taskENTER_CRITICAL();
            s_local_slot = APP_PARALLEL_STATUS_INVALID_SLOT;
            s_local_update_ms = 0UL;
            s_parallel_status_active = 0U;
            s_silent_hold_active = 0U;
            s_silent_start_ms = 0UL;
            s_local_refresh_pending = 0U;
            taskEXIT_CRITICAL();
        }
        return;
    }

    now_ms = d_timer_get_ms();
    silent_hold = zs_b1_can_protocol_is_silent() ? 1U : 0U;
    if (silent_hold != 0U)
    {
        taskENTER_CRITICAL();
        if (s_silent_hold_active == 0U)
        {
            s_silent_hold_active = 1U;
            s_silent_start_ms = now_ms;
        }
        taskEXIT_CRITICAL();
        return;
    }

    /* CAN接收路径通常已提前恢复；此处覆盖非CAN唤醒或调度竞态。 */
    if (s_silent_hold_active != 0U)
    {
        app_parallel_status_resume_after_silence();
    }

    taskENTER_CRITICAL();
    s_parallel_status_active = 1U;
    refresh_local = ((s_local_slot == APP_PARALLEL_STATUS_INVALID_SLOT) ||
                     (s_local_slot >= APP_ID_ASSIGN_MAX_PACK_COUNT) ||
                     (s_parallel_status[s_local_slot].source_address != g_ducar_can_bms_address) ||
                     (s_local_refresh_pending != 0U) ||
                     ((uint32_t)(d_timer_get_ms() - s_local_update_ms) >=
                      APP_PARALLEL_STATUS_LOCAL_REFRESH_MS)) ? 1U : 0U;
    if (refresh_local != 0U)
    {
        s_local_refresh_pending = 0U;
    }
    taskEXIT_CRITICAL();
    now_ms = d_timer_get_ms();
    /*
     * 本机状态来自应用快照而非CAN接收；首次运行、rank变化或刷新周期到期时重建本机槽位。
     * app_parallel_status_update_local会在rank变化时清理旧本机槽位，防止旧数据被误认成对端。
     */
    if (refresh_local != 0U)
    {
        app_parallel_status_update_local(now_ms);
    }

    /*
     * 对端以任一支持的B1主动PGN作为存活信号，不要求21种PGN都齐全才在线。
     * 其它对端超过5秒没有任何有效帧时清整个槽位，避免继续使用过期数据。时间差兼容计数器回绕。
     */
    for (slot = 0U; slot < APP_ID_ASSIGN_MAX_PACK_COUNT; ++slot)
    {
        uint8_t is_local;
        uint8_t is_expired;

        taskENTER_CRITICAL();
        is_local = (slot == s_local_slot) ? 1U : 0U;
        is_expired = ((silent_hold == 0U) &&
                      (s_parallel_status[slot].online != 0U) &&
                      ((uint32_t)(now_ms - s_parallel_status[slot].last_update_ms) >=
                       APP_PARALLEL_STATUS_OFFLINE_TIMEOUT_MS)) ? 1U : 0U;
        if ((is_local == 0U) && (is_expired != 0U))
        {
            app_parallel_status_clear_pack(&s_parallel_status[slot], slot);
        }
        taskEXIT_CRITICAL();

        if (is_local != 0U)
        {
            /* 本机槽位由本机快照刷新，不依据对端报文超时条件清除。 */
            continue;
        }
    }
}

/*
 * 按PGN将单帧8字节payload解码到对应包缓存。
 * pack必须是由有效BMS源地址映射得到的槽位；pgn必须在F20D~F221范围内；data指向8字节数据区。
 * 每次只覆盖当前PGN拥有的字段，其它PGN字段保持最近一次值；在线有效期在接收入口统一刷新。
 */
static void app_parallel_status_parse_frame(app_parallel_status_pack_t *pack,
                                            uint16_t pgn,
                                            const uint8_t *data)
{
    uint8_t index;
    uint8_t cell_base;

    /* 只有PGN号决定目标字段组；receive入口已保证这是支持的B1主动报文。 */
    if (pgn == 0xF20DU)
    {
        /* F20D：Data0~1总压，Data2~3电流，Data4~5 SOC，Data6 SOH，Data7 DETC状态。 */
        pack->total_voltage_001v = app_parallel_status_read_u16_le(data, 0U);
        pack->current_001a = app_parallel_status_read_s16_le(data, 2U);
        pack->soc_0p1_percent = app_parallel_status_read_u16_le(data, 4U);
        pack->soh_percent = data[6];
        pack->detc_status = data[7];
    }
    else if (pgn == 0xF20EU)
    {
        /* F20E：Data0~1最大放电电流，Data2~3最大充电电流，Data4~5/6~7单体电压极值。 */
        pack->max_discharge_current_001a = app_parallel_status_read_u16_le(data, 0U);
        pack->max_charge_current_001a = app_parallel_status_read_u16_le(data, 2U);
        pack->max_cell_mv = app_parallel_status_read_u16_le(data, 4U);
        pack->min_cell_mv = app_parallel_status_read_u16_le(data, 6U);
    }
    else if ((pgn >= 0xF20FU) && (pgn <= 0xF214U))
    {
        /* F20F至F214每帧携带4节单体；每两个Data字节为一节mV值，PGN偏移决定起始序号。 */
        cell_base = (uint8_t)((pgn - 0xF20FU) * 4U);
        for (index = 0U; index < 4U; ++index)
        {
            pack->cell_mv[cell_base + index] =
                app_parallel_status_read_u16_le(data, (uint8_t)(index * 2U));
        }
    }
    else if (pgn == 0xF215U)
    {
        /* F215：每两个Data字节为一路有符号0.1℃温度，共4路。 */
        for (index = 0U; index < 4U; ++index)
        {
            pack->cell_temp_0p1c[index] =
                app_parallel_status_read_s16_le(data, (uint8_t)(index * 2U));
        }
    }
    else if (pgn == 0xF216U)
    {
        /* F216：Data0/1为MOS/PTC有符号整数℃，Data2~5为NTC极值，Data6/7为电芯/NTC数量。 */
        pack->mos_temp_c = app_parallel_status_read_s8(data[0]);
        pack->ptc_temp_c = app_parallel_status_read_s8(data[1]);
        pack->max_cell_temp_0p1c = app_parallel_status_read_s16_le(data, 2U);
        pack->min_cell_temp_0p1c = app_parallel_status_read_s16_le(data, 4U);
        pack->cell_count = data[6];
        pack->ntc_count = data[7];
    }
    else if (pgn == 0xF217U)
    {
        /* F217：Data0~1循环次数、Data2~3满充容量、Data4~5剩余容量、Data6~7半充次数。 */
        pack->cycle_count = app_parallel_status_read_u16_le(data, 0U);
        pack->fcc_001ah = app_parallel_status_read_u16_le(data, 2U);
        pack->remaining_capacity_001ah = app_parallel_status_read_u16_le(data, 4U);
        pack->half_charge_count = app_parallel_status_read_u16_le(data, 6U);
    }
    else if (pgn == 0xF218U)
    {
        /*
         * F218 Data0 bit0..7依次为DETD、DETC、BMR（并联对端检测）、ID、放电MOS、充电MOS、预放MOS、KEY；
         * 逐位右移并与1掩码相与，统一归一为独立的0/1字段，方便后续业务直接判断。
         */
        pack->detd_present = (uint8_t)((data[0] >> 0U) & 0x01U);
        pack->detc_present = (uint8_t)((data[0] >> 1U) & 0x01U);
        pack->u8ParallelPeerDetected = (uint8_t)((data[0] >> 2U) & 0x01U);
        pack->id_present = (uint8_t)((data[0] >> 3U) & 0x01U);
        pack->discharge_mos_on = (uint8_t)((data[0] >> 4U) & 0x01U);
        pack->charge_mos_on = (uint8_t)((data[0] >> 5U) & 0x01U);
        pack->precharge_mos_on = (uint8_t)((data[0] >> 6U) & 0x01U);
        pack->key_present = (uint8_t)((data[0] >> 7U) & 0x01U);
        /* Data1~2保留协议并联MOS状态字节，Data3~5保留告警等级和工作状态。 */
        pack->parallel_mos_status[0] = data[1];
        pack->parallel_mos_status[1] = data[2];
        pack->charge_warning_level = data[3];
        pack->discharge_warning_level = data[4];
        pack->work_status = data[5];
    }
    else if (pgn == 0xF219U)
    {
        /* F219：Data0~1充电电压，Data2~3充电、Data4~5回充、Data6~7放电限流。 */
        pack->charge_voltage_001v = app_parallel_status_read_u16_le(data, 0U);
        pack->charge_current_001a = app_parallel_status_read_u16_le(data, 2U);
        pack->recharge_current_001a = app_parallel_status_read_u16_le(data, 4U);
        pack->discharge_current_001a = app_parallel_status_read_u16_le(data, 6U);
    }
    else if ((pgn >= 0xF21AU) && (pgn <= 0xF21CU))
    {
        /* 槽位0/1/2依次对应F21A/F21B/F21C，整帧复制，不改变协议位序和保留位。 */
        memcpy(pack->alarm_frame[pgn - 0xF21AU], data, D_CAN_MAX_DATA_LEN);
    }
    else if ((pgn >= 0xF21DU) && (pgn <= 0xF21FU))
    {
        /* 槽位0/1/2依次对应F21D/F21E/F21F，整帧复制，不在此处解码故障等级。 */
        memcpy(pack->protection_frame[pgn - 0xF21DU], data, D_CAN_MAX_DATA_LEN);
    }
    else if (pgn == 0xF220U)
    {
        /* F220：Data0~1控制器欠压，Data2~3过充次数，Data4~5过放次数，Data6~7剩余充电分钟。 */
        pack->controller_uv_0p1v = app_parallel_status_read_u16_le(data, 0U);
        pack->overcharge_event_count = app_parallel_status_read_u16_le(data, 2U);
        pack->overdischarge_event_count = app_parallel_status_read_u16_le(data, 4U);
        pack->charge_remaining_minutes = app_parallel_status_read_u16_le(data, 6U);
    }
    else if (pgn == 0xF221U)
    {
        /* F221：Data0为低字节的32位均衡位图，每一位置对应一个单体。 */
        pack->balance_mask = (uint32_t)data[0] |
                             ((uint32_t)data[1] << 8U) |
                             ((uint32_t)data[2] << 16U) |
                             ((uint32_t)data[3] << 24U);
    }
}

/*
 * 并联状态缓存的CAN接收入口，由CAN_ReceiveTask对队列中每帧调用。
 * 只处理：宏已启用、扩展帧、DLC=8、PGN属于F20D~F221、源地址在并联地址段内、
 * 且发送源不是本机的报文。其它帧直接忽略，不改变状态表。
 */
void app_parallel_status_receive(const t_can_packet *can_packet)
{
    uint32_t can_id;                      /* 屏蔽扩展ID中的无关标志后的29位CAN ID。 */
    uint16_t pgn;                         /* 从扩展ID的PGN高/低字节提取的16位PGN。 */
    uint8_t source_address;               /* 扩展ID最低字节中的发送BMS地址。 */
    uint8_t slot;                         /* 源地址映射得到的状态数组下标。 */
    uint8_t pgn_index;                    /* PGN映射到valid_pgn_mask的bit序号。 */
    uint32_t now_ms;                      /* 收帧时的本地毫秒计数，用于在线超时。 */
    app_parallel_status_pack_t *pack;     /* 当前源地址槽位的更新指针。 */

    /*
     * 数据有效性第一关：功能未启用、空指针、非扩展帧或DLC不为8均拒绝。
     * 当前缓存仅面向已有B1 8字节周期状态帧，不处理短帧或分段传输。
     */
    if ((app_parallel_status_is_enabled() == 0U) ||
        (can_packet == 0) ||
        (can_packet->u32idType != D_CAN_ID_EXTENDED) ||
        (can_packet->u8len != D_CAN_MAX_DATA_LEN))
    {
        return;
    }

    /*
     * 按DUCAR 29位扩展ID布局提取字段：ID[7:0]=源地址，ID[23:8]=PGN。
     * 先确认PGN受本缓存支持，未支持的扩展帧不会占用槽位或刷新在线时间。
     */
    can_id = can_packet->u32id & DUCAR_CAN_EXT_ID_MASK;
    source_address = (uint8_t)can_id;
    pgn = (uint16_t)((((can_id >> 16U) & 0xFFUL) << 8U) |
                     ((can_id >> 8U) & 0xFFUL));
    pgn_index = app_parallel_status_pgn_index(pgn);
    if (pgn_index == APP_PARALLEL_STATUS_INVALID_SLOT)
    {
        return;
    }

    /*
     * 地址有效性第二关：地址必须映射到固定连续槽位，并且不能等于本机当前地址。
     * 忽略本机回环，避免CAN硬件回读把本机报文误写为“对端”状态。
     */
    slot = app_parallel_status_slot_from_address(source_address);
    if ((slot == APP_PARALLEL_STATUS_INVALID_SLOT) ||
        (source_address == g_ducar_can_bms_address))
    {
        return;
    }

    now_ms = d_timer_get_ms();
    taskENTER_CRITICAL();
    pack = &s_parallel_status[slot];
    /*
     * 若本次帧到达前该槽位已超过离线门限，先抹掉旧PGN有效位和旧字段，
     * 再将这条新帧作为重新上线后的第一条状态。这样不会把离线前的其它字段带回来。
     */
    if ((zs_b1_can_protocol_is_silent() == false) &&
        (pack->online != 0U) &&
        ((uint32_t)(now_ms - pack->last_update_ms) >=
         APP_PARALLEL_STATUS_OFFLINE_TIMEOUT_MS))
    {
        app_parallel_status_clear_pack(pack, slot);
    }

    /*
     * 任一合法状态PGN都能刷新在线时间；同时设置该PGN的接收标志，再解码其8字节payload。
     * 右移/置位关系与头文件中的F20D~F221位表一致。
     */
    pack->online = 1U;
    pack->source_address = source_address;
    pack->last_update_ms = now_ms;
    pack->valid_pgn_mask |= (uint32_t)1UL << pgn_index;
    pack->pgn_last_update_ms[pgn_index] = now_ms;
    app_parallel_status_parse_frame(pack, pgn, can_packet->au8data);
    taskEXIT_CRITICAL();
}

uint8_t app_parallel_status_get_online_count(void)
{
    uint8_t slot;         /* 当前遍历的本机/对端地址槽位。 */
    uint8_t count = 0U;   /* 当前在线总包数，包含已刷新到表中的本机。 */

    /*
     * 本机快照刷新和对端接收都写入同一张状态表，因此直接累计全部online槽位。
     * app_can_id_assign_update在DETD通信窗口中用该总数与UID编址得到的包数核对；宏关闭时返回0。
     */
    if (app_parallel_status_is_enabled() == 0U)
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    for (slot = 0U; slot < APP_ID_ASSIGN_MAX_PACK_COUNT; ++slot)
    {
        if (s_parallel_status[slot].online != 0U)
        {
            ++count;
        }
    }
    taskEXIT_CRITICAL();
    return count;
}

uint8_t app_parallel_status_get_pack_snapshot(uint8_t slot,
                                              app_parallel_status_pack_t *pack)
{
    if ((pack == 0) ||
        (slot >= APP_ID_ASSIGN_MAX_PACK_COUNT) ||
        (app_parallel_status_is_enabled() == 0U))
    {
        return 0U;
    }

    taskENTER_CRITICAL();
    *pack = s_parallel_status[slot];
    taskEXIT_CRITICAL();
    return 1U;
}

uint8_t app_parallel_status_pgn_is_fresh(const app_parallel_status_pack_t *pack,
                                         uint8_t pgn_index,
                                         uint32_t max_age_ms)
{
    uint32_t now_ms;

    if ((pack == 0) || (pgn_index >= 21U) || (pack->online == 0U) ||
        ((pack->valid_pgn_mask & ((uint32_t)1UL << pgn_index)) == 0UL))
    {
        return 0U;
    }

    now_ms = d_timer_get_ms();
    return ((uint32_t)(now_ms - pack->pgn_last_update_ms[pgn_index]) <= max_age_ms) ?
           1U : 0U;
}

const app_parallel_status_pack_t *app_parallel_status_get_pack(uint8_t slot)
{
    /*
     * 只读查询固定地址槽位，调用方可检查online、valid_pgn_mask后再读取字段。
     * 返回指向静态缓存的指针；指针本身长期有效，但字段会随CAN接收/task刷新而变化。
     * 本机rank映射到的槽位保存本机快照；其它槽位保存对应CAN源地址的对端状态。
     * 越界返回NULL，不做地址推算或槽位创建。
     */
    if ((app_parallel_status_is_enabled() == 0U) ||
        (slot >= APP_ID_ASSIGN_MAX_PACK_COUNT))
    {
        return 0;
    }
    return &s_parallel_status[slot];
}
