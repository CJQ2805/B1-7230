/******************************************************************************
* 文件名称: SID27_SecurityAccess.c
* 内容摘要: 安全访问
* 创建者の: 孔佳伟
* 个人主页: https://gitee.com/openes
* 修改记录: 
******************************************************************************/

#include "SID27_SecurityAccess.h"
#include "service_cfg.h"
#include "uds_service.h"
#include "SID10_SessionControl.h"
#include "uds_port.h"
#include "uds_customer_profile.h"
#include <string.h>

#define UDS_SEED_LENGTH                   (0x04)
#define UDS_LV1_REQUEST_SEED              (0x01)
#define UDS_LV1_SEND_KEY                  (0x02)
#define UDS_LV2_REQUEST_SEED              (0x09)
#define UDS_LV2_SEND_KEY                  (0x0A)
#define UDS_FAS_MAX_TIMES                 (0x02)  /* failed security access */

static uint8_t req_seed = 0U;             // 仅允许紧接对应的Seed请求提交Key。
static uint8_t pending_seed_subfunction = 0U;
static uds_sa_lv pending_security_level = UDS_SA_NON;
static uint8_t org_seed_buf[UDS_SEED_LENGTH];
static uint32_t s_seed_state = 0U;         // 保留既有seed生成方式；该算法不是密码学随机数。


// 当前安全访问等级
static uds_sa_lv curr_sa = UDS_SA_NON;

// 安全访问种子匹配错误次数
uint8_t uds_fsa_cnt = 0;

/******************************************************************************
* 函数名称: void set_current_sa_lv(uds_sa_lv level)
* 功能说明: 设置当前安全访问等级
* 输入参数: uds_sa_lv level        	--安全访问等级
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void set_current_sa_lv(uds_sa_lv level)
{
	curr_sa = level;
}


/******************************************************************************
* 函数名称: uds_session_t get_current_sa_lv(void)
* 功能说明: 获取当前安全访问等级
* 输入参数: 无
* 输出参数: 无
* 函数返回: 当前安全访问等级
* 其它说明: 无
******************************************************************************/
uds_sa_lv get_current_sa_lv(void)
{
    return curr_sa;
}

/* 切换诊断会话时丢弃旧seed，不能把上一会话的seed拿来提交key。 */
void uds_security_reset(void)
{
    req_seed = 0U;
    pending_seed_subfunction = 0U;
    pending_security_level = UDS_SA_NON;
    memset(org_seed_buf, 0, sizeof(org_seed_buf));
    curr_sa = UDS_SA_NON;
}

/* 客户规定：27 01/02 属于扩展会话一级，27 09/0A 属于编程会话二级。 */
static bool_t security_subfunction_for_session(uint8_t subfunction,
                                               uds_sa_lv *level_out,
                                               bool_t *is_seed_out)
{
    uds_session_t session = get_current_session();

    if ((level_out == NULL) || (is_seed_out == NULL))
    {
        return FALSE;
    }

    if (session == UDS_SESSION_EXT)
    {
        if ((subfunction == UDS_LV1_REQUEST_SEED) ||
            (subfunction == UDS_LV1_SEND_KEY))
        {
            *level_out = UDS_SA_LV1;
            *is_seed_out = (subfunction == UDS_LV1_REQUEST_SEED) ? TRUE : FALSE;
            return TRUE;
        }
    }
    else if (session == UDS_SESSION_PROG)
    {
        if ((subfunction == UDS_LV2_REQUEST_SEED) ||
            (subfunction == UDS_LV2_SEND_KEY))
        {
            *level_out = UDS_SA_LV2;
            *is_seed_out = (subfunction == UDS_LV2_REQUEST_SEED) ? TRUE : FALSE;
            return TRUE;
        }
    }

    return FALSE;
}


/******************************************************************************
* 函数名称: static uint8_t rand_u8 (void)
* 功能说明: 获取随机数
* 输入参数: 无
* 输出参数: 无
* 函数返回: 8 位随机数
* 其它说明: 无
******************************************************************************/
static uint8_t rand_u8(void)
{
    /*
     * 协议约定的seed/key是简单异或校验，不是密码学安全访问。
     * 用1ms单调计时与滚动状态避免当前空函数造成未定义返回值。
     */
    if (s_seed_state == 0U)
    {
        s_seed_state = uds_port_get_time_ms() ^ 0xA5366B4DU;
        if (s_seed_state == 0U)
        {
            s_seed_state = 0x6D2B79F5U;
        }
    }

    s_seed_state ^= (s_seed_state << 13);
    s_seed_state ^= (s_seed_state >> 17);
    s_seed_state ^= (s_seed_state << 5);
    return (uint8_t)(s_seed_state & 0xFFU);
}


/******************************************************************************
* 函数名称: static uint32_t seedTOKey(uint32_t seed)
* 功能说明: 安全访问算法
* 输入参数: uint32_t seed		--种子
* 输出参数: 无
* 函数返回: key 值
* 其它说明: 该算法需根据实际需求而定
******************************************************************************/
/******************************************************************************
* 函数名称: int uds_security_access(uint8_t* key_buf, uint8_t* seed_buf)
* 功能说明: 比较自己根据种子 seed 计算的 key 值与接收到的 key 值是否一致
* 输入参数: uint8_t* key_buf		--接收到的 key
    　　　　uint8_t* seed_buf		--种子
* 输出参数: 无
* 函数返回: 0: 一致; -1: 不一致
* 其它说明: 无
******************************************************************************/
int uds_security_access(uint8_t* key_buf, uint8_t* seed_buf, uint8_t security_level)
{
	uint8_t expected_key[UDS_SEED_LENGTH];
	uint8_t index;
	const uint8_t *key_const;

	if ((key_buf == NULL) || (seed_buf == NULL))
	{
		return -1;
	}
	key_const = uds_customer_profile_get_key_const(security_level);
	if (key_const == NULL)
	{
		return -1;
	}

	uds_customer_profile_calculate_key(seed_buf,
	                                   key_const,
	                                   expected_key);
	for (index = 0U; index < UDS_SEED_LENGTH; ++index)
	{
		if (key_buf[index] != expected_key[index])
		{
			return -1;
		}
	}
	return 0;
}


/******************************************************************************
* 函数名称: bool_t service_27_check_len(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 检查 27 服务数据长度是否合法
* 输入参数: uint16_t msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: TRUE: 合法; FALSE: 非法
* 其它说明: 无
******************************************************************************/
bool_t service_27_check_len(const uint8_t* msg_buf, uint16_t msg_dlc)
{
	uint8_t subfunction;

	if ((msg_buf == NULL) || (msg_dlc < 2U))
	{
		return FALSE;
	}
	subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);
	return ((((UDS_LV1_REQUEST_SEED == subfunction) ||
	          (UDS_LV2_REQUEST_SEED == subfunction)) && (msg_dlc == 2U)) ||
	        (((UDS_LV1_SEND_KEY == subfunction) ||
	          (UDS_LV2_SEND_KEY == subfunction)) && (msg_dlc == 6U))) ? TRUE : FALSE;
}


/******************************************************************************
* 函数名称: void service_27_SecurityAccess(const uint8_t* msg_buf, uint16_t msg_dlc)
* 功能说明: 27 服务 - 安全访问
* 输入参数: uint8_t*    msg_buf         --数据首地址
    　　　　uint8_t     msg_dlc         --数据长度
* 输出参数: 无
* 函数返回: 无
* 其它说明: 无
******************************************************************************/
void service_27_SecurityAccess(const uint8_t* msg_buf, uint16_t msg_dlc)
{
    uint8_t subfunction;
	uint8_t rsp_buf[UDS_SEED_LENGTH + 2U];
	uint16_t i;
	uds_sa_lv requested_level;
	bool_t is_seed_request;
	uint8_t expected_key_subfunction;

	if ((msg_buf == NULL) || (msg_dlc < 2U) ||
	    !service_27_check_len(msg_buf, msg_dlc))
	{
		uds_negative_rsp(SID_27, NRC_INVALID_MESSAGE_LENGTH_OR_FORMAT);
		return;
	}

	subfunction = UDS_GET_SUB_FUNCTION(msg_buf[1]);
	if (!security_subfunction_for_session(subfunction, &requested_level, &is_seed_request))
	{
		uds_negative_rsp(SID_27, NRC_SUBFUNCTION_NOT_SUPPORTED);
		return;
	}

	if (is_seed_request == TRUE)
	{
		/* 新Seed请求废弃旧Seed，避免跨请求、跨等级提交旧Key。 */
		req_seed = 0U;
		pending_seed_subfunction = 0U;
		pending_security_level = UDS_SA_NON;
		memset(org_seed_buf, 0, sizeof(org_seed_buf));

		if (uds_timer_chk(UDS_TIMER_FSA) > 0)
		{
			uds_negative_rsp(SID_27, NRC_REQUIRED_TIME_DELAY_NOT_EXPIRED);
			return;
		}

		rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_27);
		rsp_buf[1] = subfunction;
		for (i = 0U; i < UDS_SEED_LENGTH; ++i)
		{
			/* 已达到或超过目标等级时，按UDS约定返回全零Seed。 */
			org_seed_buf[i] = (curr_sa >= requested_level) ? 0U : rand_u8();
			rsp_buf[2U + i] = org_seed_buf[i];
		}

		if (uds_positive_rsp(rsp_buf, (uint16_t)sizeof(rsp_buf)) == 0)
		{
			req_seed = 1U;
			pending_seed_subfunction = subfunction;
			pending_security_level = requested_level;
		}
		else
		{
			/* 若Seed响应未提交给传输层，不保留对应Key的授权上下文。 */
			memset(org_seed_buf, 0, sizeof(org_seed_buf));
		}
		return;
	}

	/* Key必须与同一会话中紧邻的同等级Seed请求配对。 */
	expected_key_subfunction = (uint8_t)(pending_seed_subfunction + 1U);
	if ((req_seed == 0U) ||
	    (subfunction != expected_key_subfunction) ||
	    (requested_level != pending_security_level))
	{
		req_seed = 0U;
		pending_seed_subfunction = 0U;
		pending_security_level = UDS_SA_NON;
		uds_negative_rsp(SID_27, NRC_REQUEST_SEQUENCE_ERROR);
		return;
	}
	requested_level = pending_security_level;

	if (uds_security_access((uint8_t *)&msg_buf[2],
	                        org_seed_buf,
	                        pending_seed_subfunction) == 0)
	{
		rsp_buf[0] = USD_GET_POSITIVE_RSP(SID_27);
		rsp_buf[1] = subfunction;
		/* 只有肯定响应已提交后才开放对应等级，避免静默授权。 */
		if (uds_positive_rsp(rsp_buf, 2U) == 0)
		{
			req_seed = 0U;
			pending_seed_subfunction = 0U;
			pending_security_level = UDS_SA_NON;
			set_current_sa_lv(requested_level);
			uds_fsa_cnt = 0U;
			memset(org_seed_buf, 0, sizeof(org_seed_buf));
		}
	}
	else
	{
		/* 错误Key消耗本次Seed，不能对同一个Seed重复猜测。 */
		req_seed = 0U;
		pending_seed_subfunction = 0U;
		pending_security_level = UDS_SA_NON;
		uds_fsa_cnt++;
		memset(org_seed_buf, 0, sizeof(org_seed_buf));
		if (uds_fsa_cnt >= UDS_FAS_MAX_TIMES)
		{
			uds_timer_start(UDS_TIMER_FSA);
			uds_negative_rsp(SID_27, NRC_EXCEEDED_NUMBER_OF_ATTEMPTS);
		}
		else
		{
			uds_negative_rsp(SID_27, NRC_INVALID_KEY);
		}
	}
}


/****************EOF****************/
