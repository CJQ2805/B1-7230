#include "App/app_id_assign.h"

#include <string.h>

#include "Driver/d_can.h"
#include "Driver/d_timer.h"
#include "Protocol/Comm/ducar_can_protocol.h"
#include "n32g45x.h"

#define ID_ASSIGN_MSG_SEND_TIME_MS       ((uint32_t)1000UL) /* 启动编址后持续广播UID并收集对端1秒。 */
#define ID_ASSIGN_REASSIGN_CHECK_MS      ((uint32_t)5000UL) /* 编址稳定后每5秒比较在线包数与已收集UID数量。 */
#define ID_ASSIGN_RESTART_GUARD_MS       ((uint32_t)500UL)  /* 新rank生效后的500ms内忽略旧编址广播。 */
#define ID_ASSIGN_MSG_CYCLE_MS           ((uint32_t)50UL)   /* UID编址广播周期50ms。 */
#define ID_ASSIGN_MSG_LENGTH             ((uint8_t)8U)      /* UID广播携带8字节，即96位UID中的前64位。 */
#define ID_ASSIGN_UID_ALTERNATE_ADDR     ((uint32_t)0x1FFFF270UL) /* 项目使用的备用UID存储地址。 */

volatile ID_ASSIGN_E id_assign_state = ID_ASSIGN_IDLE;

static uint8_t s_peer_uid[APP_ID_ASSIGN_PEER_CAPACITY][ID_ASSIGN_MSG_LENGTH]; /* 本轮收集到的不同对端UID列表。 */
static uint8_t s_peer_count;                    /* 已缓存对端UID数，不包含本机UID。 */
static uint8_t s_local_uid[ID_ASSIGN_MSG_LENGTH]; /* 本机UID的8字节编址表示。 */
static uint8_t s_address;                       /* 本机rank：比本机UID小的UID数量。 */
static uint32_t s_state_start_ms;               /* 当前编址收集阶段的启动时刻。 */
static uint32_t s_last_tx_ms;                   /* 最近一次发送UID广播的时刻。 */
static uint32_t s_restart_guard_ms;              /* 最近一次rank确认时刻，供短暂保护窗判断。 */
static uint32_t s_reassign_check_ms;             /* 最近一次在线成员数量复核时刻。 */

/* 读取MCU 96位UID中的前64位，并按UID编址报文规定的高字节在前顺序保存。 */
static void app_id_assign_read_uid(uint8_t *uid)
{
    const volatile uint32_t *uid_words =
        (const volatile uint32_t *)ID_ASSIGN_UID_ALTERNATE_ADDR;
    uint32_t word0;
    uint32_t word1;

    /* 与厂商GetUID()一致：备用地址未烧录时从UID_BASE读取。 */
    if (uid_words[0] == 0xFFFFFFFFUL)
    {
        uid_words = (const volatile uint32_t *)UID_BASE;
    }

    word0 = uid_words[0];
    word1 = uid_words[1];

    uid[0] = (uint8_t)(word0 >> 24U);
    uid[1] = (uint8_t)(word0 >> 16U);
    uid[2] = (uint8_t)(word0 >> 8U);
    uid[3] = (uint8_t)word0;
    uid[4] = (uint8_t)(word1 >> 24U);
    uid[5] = (uint8_t)(word1 >> 16U);
    uid[6] = (uint8_t)(word1 >> 8U);
    uid[7] = (uint8_t)word1;
}

static bool app_id_assign_uid_equal(const uint8_t *left, const uint8_t *right)
{
    /* UID按广播载荷逐字节完全相同才视为同一设备。 */
    return (memcmp(left, right, ID_ASSIGN_MSG_LENGTH) == 0);
}

/* UID按报文中的字节序逐字节比较，避免用字节和比较造成排序碰撞。 */
static bool app_id_assign_uid_less(const uint8_t *left, const uint8_t *right)
{
    uint8_t index;

    for (index = 0U; index < ID_ASSIGN_MSG_LENGTH; ++index)
    {
        if (left[index] < right[index])
        {
            return true;
        }
        if (left[index] > right[index])
        {
            return false;
        }
    }

    return false;
}

static void app_id_assign_send_uid(void)
{
    /* 发送扩展帧ID 0x18F10815；Data0~7为本机UID前64位，供总线上各包共同排序。 */
    (void)d_can_send(&can0,
                     ID_ASSIGN_MSG,
                     D_CAN_ID_EXTENDED,
                     s_local_uid,
                     ID_ASSIGN_MSG_LENGTH);
}

static uint8_t app_id_assign_calculate_rank(void)
{
    uint8_t index;
    uint8_t rank = 0U;

    /* UID按字节序升序排列；rank等于排在本机之前的不同对端数量。 */
    for (index = 0U; index < s_peer_count; ++index)
    {
        if (app_id_assign_uid_less(s_peer_uid[index], s_local_uid))
        {
            ++rank;
        }
    }

    return rank;
}

static void app_id_assign_reset_collection(void)
{
    /* 新一轮编址从空成员表开始；本机UID单独保存，不计入s_peer_count。 */
    memset(s_peer_uid, 0, sizeof(s_peer_uid));
    s_peer_count = 0U;
}

/* 收到静默期内的UID帧代表总线成员活动：立即重开收集窗口并保留这条触发UID。 */
void app_id_assign_restart_with_frame(const t_can_packet *can_packet)
{
    uint32_t now_ms;

    if ((can_packet == 0) ||
        (can_packet->u32idType != D_CAN_ID_EXTENDED) ||
        (can_packet->u32id != ID_ASSIGN_MSG) ||
        (can_packet->u8len != ID_ASSIGN_MSG_LENGTH))
    {
        return;
    }

    app_id_assign_reset_collection();
    app_id_assign_read_uid(s_local_uid);
    now_ms = d_timer_get_ms();
    s_state_start_ms = now_ms;
    s_last_tx_ms = now_ms;
    s_restart_guard_ms = now_ms;
    s_reassign_check_ms = now_ms;
    id_assign_state = ID_ASSIGN_MSG_SEND;
    app_id_assign_send_uid();

    /* 触发本轮重编址的UID帧也属于新收集窗口，避免丢掉唯一一条广播。 */
    app_id_assign_msg_handle(can_packet);
}

void app_id_assign_msg_handle(const t_can_packet *can_packet)
{
    uint8_t index;

    if ((can_packet == 0) ||
        (can_packet->u32idType != D_CAN_ID_EXTENDED) ||
        (can_packet->u32id != ID_ASSIGN_MSG) ||
        (can_packet->u8len != ID_ASSIGN_MSG_LENGTH))
    {
        return;
    }

    /*
     * 仅接受指定扩展帧ID且DLC为8的UID广播。收集窗口内忽略本机回环和重复UID；
     * 编址完成后，保护窗过期再收到UID广播则将状态退回IDLE，由调度任务重开收集。
     */
    if (id_assign_state == ID_ASSIGN_MSG_SEND)
    {
        if (app_id_assign_uid_equal(can_packet->au8data, s_local_uid))
        {
            return;
        }

        for (index = 0U; index < s_peer_count; ++index)
        {
            if (app_id_assign_uid_equal(can_packet->au8data, s_peer_uid[index]))
            {
                return;
            }
        }

        if (s_peer_count < APP_ID_ASSIGN_PEER_CAPACITY)
        {
            memcpy(s_peer_uid[s_peer_count],
                   can_packet->au8data,
                   ID_ASSIGN_MSG_LENGTH);
            ++s_peer_count;
        }
    }
    else if ((id_assign_state == ID_ASSIGNED) &&
             ((uint32_t)(d_timer_get_ms() - s_restart_guard_ms) >=
              ID_ASSIGN_RESTART_GUARD_MS))
    {
        id_assign_state = ID_ASSIGN_IDLE;
    }
    else
    {
        /* 闲置或保护窗口内的编址帧不参与本轮UID收集。 */
    }
}

uint8_t app_id_assign_task(bool enable, uint8_t parallel_num)
{
    uint32_t now_ms = d_timer_get_ms();

    if (!enable)
    {
        /* 调用方要求暂停时回到IDLE；保留已计算的s_address供调用方决定是否沿用。 */
        id_assign_state = ID_ASSIGN_IDLE;
        s_reassign_check_ms = now_ms;
        return s_address;
    }

    switch (id_assign_state)
    {
        case ID_ASSIGN_IDLE:
            /* IDLE启动新一轮：清空对端集合、读取本机UID并立即发第一帧。 */
            app_id_assign_reset_collection();
            app_id_assign_read_uid(s_local_uid);
            s_state_start_ms = now_ms;
            s_last_tx_ms = now_ms;
            app_id_assign_send_uid();
            id_assign_state = ID_ASSIGN_MSG_SEND;
            break;

        case ID_ASSIGN_MSG_SEND:
            /* MSG_SEND阶段每50ms广播一次；达到1秒收集窗口后转入rank计算阶段。 */
            if ((uint32_t)(now_ms - s_state_start_ms) >=
                ID_ASSIGN_MSG_SEND_TIME_MS)
            {
                id_assign_state = ID_ASSIGNING;
            }
            else if ((uint32_t)(now_ms - s_last_tx_ms) >= ID_ASSIGN_MSG_CYCLE_MS)
            {
                app_id_assign_send_uid();
                s_last_tx_ms = now_ms;
            }
            break;

        case ID_ASSIGNING:
            /* 按已收集UID集合计算唯一rank，并开始短保护窗及在线包数复核计时。 */
            s_address = app_id_assign_calculate_rank();
            s_restart_guard_ms = now_ms;
            s_reassign_check_ms = now_ms;
            id_assign_state = ID_ASSIGNED;
            break;

        case ID_ASSIGNED:
            /* 稳定阶段周期核对在线包数；非零在线数与已收集总包数不符时重启编址。 */
            if ((uint32_t)(now_ms - s_reassign_check_ms) >=
                ID_ASSIGN_REASSIGN_CHECK_MS)
            {
                s_reassign_check_ms = now_ms;
                if ((parallel_num != 0U) &&
                    (parallel_num != (uint8_t)(s_peer_count + 1U)))
                {
                    id_assign_state = ID_ASSIGN_IDLE;
                }
            }
            break;

        case ID_MAX:
        default:
            id_assign_state = ID_ASSIGN_IDLE;
            break;
    }

    return s_address;
}
