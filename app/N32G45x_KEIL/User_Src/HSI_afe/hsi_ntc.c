/*
 * 文件作用：
 *   NTC温度表模块，提供NTC电阻或ADC换算到温度所需的查表数据。
 *
 * 主要用途：
 *   1. 保存当前NTC规格书对应的温度映射表。
 *   2. 给AFE温度换算流程提供查表依据。
 *
 * 适用范围：
 *   适用于当前项目选用的NTC型号。更换NTC规格书时应更新本文件中的映射表。
 *
 * 调用关系：
 *   hsi_afe_dvc1124.c 或 hsi_afe.c 在温度换算时引用表数据。
 *
 * 注意事项：
 *   表数据属于硬件物料参数，不应在故障、通信或LOG模块中重复定义。
 */
#include "hsi_ntc.h"

/*
 * 电芯/电池包外部NTC阻温表，来源于TDK NTCG163JF103FT1S规格书。
 * 每项为 {阻值Ω, 温度0.1℃}，例如 {10000, 250} 表示10kΩ对应25.0℃。
 * 阻值按升序排列、温度按降序排列，hsi_ntc_get_temp()依赖此顺序查找区间。
 * 表格约覆盖-40.0℃至150.0℃；范围之外的阻值会钳位到端点温度。
 * 这里直接存储规格书阻温点，所以外部NTC温度计算不单独使用B值公式。
 */
hsi_ntc_map_t gt_hsi_ntc_map_1[] =
{
    /* TDK NTCG163JF103FT1S标称阻温表：x为欧姆，y为0.1摄氏度。 */
    {   313,  1500}, {   346,  1450}, {   385,  1400},
    {   428,  1350}, {   478,  1300}, {   534,  1250},
    {   599,  1200}, {   674,  1150}, {   760,  1100},
    {   860,  1050}, {   975,  1000}, {  1110,   950},
    {  1267,   900}, {  1451,   850}, {  1668,   800},
    {  1924,   750}, {  2227,   700}, {  2588,   650},
    {  3019,   600}, {  3536,   550}, {  4158,   500},
    {  4911,   450}, {  5826,   400}, {  6940,   350},
    {  8310,   300}, { 10000,   250}, { 12090,   200},
    { 14700,   150}, { 17960,   100}, { 22070,    50},
    { 27280,     0}, { 33930,   -50}, { 42450,  -100},
    { 53460,  -150}, { 67800,  -200}, { 86600,  -250},
    {111300,  -300}, {144300,  -350}, {188500,  -400},
};

/* AFE GP温度通道使用同一颗NTC规格，表格单位与gt_hsi_ntc_map_1相同。 */
hsi_ntc_map_t gt_hsi_ntc_map_2[] =
{
    /* AFE GP温度通道同样使用这张阻温表。 */
    {   313,  1500}, {   346,  1450}, {   385,  1400},
    {   428,  1350}, {   478,  1300}, {   534,  1250},
    {   599,  1200}, {   674,  1150}, {   760,  1100},
    {   860,  1050}, {   975,  1000}, {  1110,   950},
    {  1267,   900}, {  1451,   850}, {  1668,   800},
    {  1924,   750}, {  2227,   700}, {  2588,   650},
    {  3019,   600}, {  3536,   550}, {  4158,   500},
    {  4911,   450}, {  5826,   400}, {  6940,   350},
    {  8310,   300}, { 10000,   250}, { 12090,   200},
    { 14700,   150}, { 17960,   100}, { 22070,    50},
    { 27280,     0}, { 33930,   -50}, { 42450,  -100},
    { 53460,  -150}, { 67800,  -200}, { 86600,  -250},
    {111300,  -300}, {144300,  -350}, {188500,  -400},
};

/*
 * 将NTC阻值换算为温度，输出单位为0.1℃。
 * 先识别调用者传入的表并取得表长；未知表指针沿用历史兜底值105.0℃。
 * 表内阻值按升序排列，因此可在相邻两个表点之间作线性插值。
 */
int16_t hsi_ntc_get_temp(hsi_ntc_map_t *ptNtcMap, uint32_t u32ResistanceOhm)
{
    uint8_t u8MapIndex;
    uint8_t u8MapSize = 0U;
    int32_t s32InterpolatedTemperature;

    if(ptNtcMap == gt_hsi_ntc_map_1)
    {
        u8MapSize = sizeof(gt_hsi_ntc_map_1) / sizeof(hsi_ntc_map_t);
    }
    else if(ptNtcMap == gt_hsi_ntc_map_2)
    {
        u8MapSize = sizeof(gt_hsi_ntc_map_2) / sizeof(hsi_ntc_map_t);
    }

    /* 仅接受本模块登记的两张表，避免对未知指针继续索引。 */
    if(u8MapSize == 0U)
    {
        return 1050;
    }

    /* 阻值低于最小表点时按最高温钳位。 */
    if(u32ResistanceOhm <= (uint32_t)ptNtcMap[0].x)
    {
        return (int16_t)ptNtcMap[0].y;
    }

    /* 阻值高于最大表点时按最低温钳位。 */
    if(u32ResistanceOhm >= (uint32_t)ptNtcMap[u8MapSize - 1U].x)
    {
        return (int16_t)ptNtcMap[u8MapSize - 1U].y;
    }

    /* 找到第一个阻值大于或等于输入值的点；前一个点与它构成插值区间。 */
    for(u8MapIndex = 0U; u8MapIndex < u8MapSize; u8MapIndex++)
    {
        if((uint32_t)ptNtcMap[u8MapIndex].x >= u32ResistanceOhm)
        {
            break;
        }
    }

    /*
     * 线性插值：T = T0 + (R-R0)*(T1-T0)/(R1-R0)，0为前一个表点，1为当前表点。
     * 阻值升高时NTC温度下降，所以T1-T0通常为负。
     * 整数除法会舍弃小数部分，最终温度仍以0.1℃为一个计数单位。
     */
    s32InterpolatedTemperature = ((int32_t)u32ResistanceOhm - ptNtcMap[u8MapIndex - 1U].x) * (ptNtcMap[u8MapIndex].y - ptNtcMap[u8MapIndex - 1U].y);
    if((ptNtcMap[u8MapIndex].x - ptNtcMap[u8MapIndex - 1U].x) != 0)
    {
        s32InterpolatedTemperature = s32InterpolatedTemperature / (ptNtcMap[u8MapIndex].x - ptNtcMap[u8MapIndex - 1U].x);
    }
    s32InterpolatedTemperature += ptNtcMap[u8MapIndex - 1U].y;

    return (int16_t)s32InterpolatedTemperature;
}
