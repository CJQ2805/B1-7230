#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "App/app_current_calib.h"
#include "App/app_flash_map.h"
#include "HSI_afe/hsi_afe.h"

uint8_t g_test_flash[2048];
static uint8_t s_cal2_page_a[2048];
static uint8_t s_cal2_page_b[2048];
hsi_afe_t gt_hsi_afe;

static uint32_t s_last_flash_offset;
static uint32_t s_last_flash_length;
static uint32_t s_flash_write_count;
static uint8_t s_fail_next_commit;
static uint32_t s_critical_enter_count;
static uint8_t s_critical_depth;

void app_current_calib_test_critical_enter(void)
{
    ++s_critical_enter_count;
    ++s_critical_depth;
}

void app_current_calib_test_critical_exit(void)
{
    assert(s_critical_depth != 0U);
    --s_critical_depth;
}

const uint8_t *app_current_calib_test_flash_ptr(uint32_t addr)
{
    if (addr >= APP_FLASH_MAP_DB_PARAM_ADDR && addr < APP_FLASH_MAP_DB_PARAM_ADDR + 2048U)
        return &g_test_flash[addr - APP_FLASH_MAP_DB_PARAM_ADDR];
    if (addr >= APP_FLASH_MAP_CAL2_A_ADDR && addr < APP_FLASH_MAP_CAL2_A_ADDR + 2048U)
        return &s_cal2_page_a[addr - APP_FLASH_MAP_CAL2_A_ADDR];
    if (addr >= APP_FLASH_MAP_CAL2_B_ADDR && addr < APP_FLASH_MAP_CAL2_B_ADDR + 2048U)
        return &s_cal2_page_b[addr - APP_FLASH_MAP_CAL2_B_ADDR];
    return 0;
}

bool d_flash_acquire(uint32_t timeout_ms) { (void)timeout_ms; return true; }
void d_flash_release(void) {}
bool d_flash_is_blank(uint32_t addr, uint32_t len)
{
    const uint8_t *p = app_current_calib_test_flash_ptr(addr);
    uint32_t i;
    if (p == 0) return false;
    for (i = 0U; i < len; ++i) if (p[i] != 0xFFU) return false;
    return true;
}
bool d_flash_erase_page(uint32_t addr)
{
    uint8_t *p = (uint8_t *)app_current_calib_test_flash_ptr(addr);
    if (p == 0 || (addr & 2047U) != 0U) return false;
    memset(p, 0xFF, 2048U);
    return true;
}
bool d_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint8_t *p = (uint8_t *)app_current_calib_test_flash_ptr(addr);
    uint32_t i;
    if (p == 0 || !d_flash_is_blank(addr, len)) return false;
    if (len == 4U && s_fail_next_commit != 0U)
    {
        s_fail_next_commit = 0U;
        return false;
    }
    for (i = 0U; i < len; ++i) p[i] = data[i];
    s_flash_write_count++;
    return true;
}

bool app_param_flash_page_update(uint32_t offset, const uint8_t *data, uint32_t len)
{
    if ((data == 0) || ((offset + len) > (uint32_t)sizeof(g_test_flash)))
    {
        return false;
    }

    memcpy(&g_test_flash[offset], data, len);
    s_last_flash_offset = offset;
    s_last_flash_length = len;
    s_flash_write_count++;
    return true;
}

static void sample_calibration_point(int32_t target_ma, int32_t raw_ma)
{
    uint8_t sample_index;

    assert(app_current_calib_submit(target_ma));
    assert(app_current_calib_is_busy());
    for (sample_index = 0U; sample_index < APP_CURRENT_CALIB_SAMPLE_COUNT; sample_index++)
    {
        gt_hsi_afe.curr_raw = raw_ma;
        gt_hsi_afe.current_update_seq++;
        app_current_calib_task();
    }
    assert(!app_current_calib_is_busy());
}

static void assert_shared_calibration(uint32_t expected_k_ppm, int32_t expected_b_ma)
{
    uint32_t k_ppm;
    int32_t b_ma;

    gt_hsi_afe.curr_raw = 20000;
    app_current_calib_task();
    assert(gt_hsi_afe.curr_calibrated ==
           app_current_calib_calculate_output(expected_k_ppm, expected_b_ma, 20000));
    assert(app_current_calib_get(&k_ppm, &b_ma));
    assert(k_ppm == expected_k_ppm);
    assert(b_ma == expected_b_ma);

    gt_hsi_afe.curr_raw = -19900;
    app_current_calib_task();
    assert(gt_hsi_afe.curr_calibrated ==
           app_current_calib_calculate_output(expected_k_ppm, expected_b_ma, -19900));
    assert(app_current_calib_get(&k_ppm, &b_ma));
    assert(k_ppm == expected_k_ppm);
    assert(b_ma == expected_b_ma);
}

int main(void)
{
    uint32_t k_ppm;
    int32_t b_ma;
    uint8_t status;

    memset(g_test_flash, 0xFF, sizeof(g_test_flash));
    memset(s_cal2_page_a, 0xFF, sizeof(s_cal2_page_a));
    memset(s_cal2_page_b, 0xFF, sizeof(s_cal2_page_b));
    memset(&gt_hsi_afe, 0, sizeof(gt_hsi_afe));
    app_current_calib_init();

    /* 0A is optional: a known positive point is accepted against default R0=0. */
    assert(app_current_calib_get(&k_ppm, &b_ma));
    assert(k_ppm == APP_CURRENT_CALIB_K_DEFAULT_PPM);
    assert(b_ma == 0);
    sample_calibration_point(10000, 20000);
    assert_shared_calibration(500000U, 0);

    /* An optional zero point updates the shared offset, not a direction-specific pair. */
    sample_calibration_point(0, 100);
    assert_shared_calibration(500000U, -50);

    /* A negative known point produces the same shared K/B for both current polarities. */
    sample_calibration_point(-10000, -19900);
    assert_shared_calibration(500000U, -50);
    assert(s_flash_write_count == 6U);
    assert(s_cal2_page_a[0] == 'C');
    assert(s_cal2_page_a[1] == 'A');

    /* A reboot restores the same common K/B from the committed journal record. */
    app_current_calib_init();
    assert_shared_calibration(500000U, -50);

    /* CAL2 stores no candidate until both independent, stable points complete. */
    gt_hsi_afe.data_ready = 1U;
    gt_hsi_afe.status_valid = 1U;
    gt_hsi_afe.curr_reg = 100;
    gt_hsi_afe.curr_raw = 1124;
    gt_hsi_afe.current_update_seq++;
    app_current_calib_task();
    {
        uint32_t critical_count_before = s_critical_enter_count;
        status = app_current_calib_cal2_request(1U, 1U, 0x1234U, 0, 0x3AU);
        assert(status == 3U);
        assert(s_critical_enter_count > critical_count_before);
        assert(s_critical_depth == 0U);
    }
    status = app_current_calib_cal2_request(2U, 2U, 0x1234U, 1000, 0x3AU);
    assert(status == 0U);
    assert(app_current_calib_cal2_request(2U, 2U, 0x1234U, 1000, 0x3AU) == 0U);
    assert(app_current_calib_cal2_request(2U, 2U, 0x1234U, 1001, 0x3AU) == 1U);
    for (uint8_t i = 0U; i < 5U; ++i)
    {
        app_current_calib_cal2_advance_ms(700U);
        gt_hsi_afe.curr_raw = 1124;
        gt_hsi_afe.current_update_seq++;
        app_current_calib_task();
    }
    assert(app_current_calib_cal2_request(5U, 3U, 0x1234U, 0, 0x3AU) == 0x0DU);
    assert_shared_calibration(500000U, -50);
    assert(app_current_calib_cal2_request(3U, 4U, 0x1234U, 21000, 0x3AU) == 0U);
    for (uint8_t i = 0U; i < 5U; ++i)
    {
        app_current_calib_cal2_advance_ms(700U);
        gt_hsi_afe.curr_raw = 21124;
        gt_hsi_afe.current_update_seq++;
        app_current_calib_task();
    }
    assert(app_current_calib_cal2_request(5U, 5U, 0x1234U, 0, 0x3AU) == 3U);
    assert_shared_calibration(1000000U, -124);
    assert(app_current_calib_cal2_request(3U, 4U, 0x1234U, 21000, 0x3AU) == 3U);
    app_current_calib_init();
    assert_shared_calibration(1000000U, -124);

    /* An interrupted commit leaves the last complete pair effective, even after reboot. */
    gt_hsi_afe.curr_raw = 10000;
    gt_hsi_afe.current_update_seq++;
    app_current_calib_task();
    assert(app_current_calib_cal2_request(1U, 1U, 0x1235U, 0, 0x3AU) == 3U);
    assert(app_current_calib_cal2_request(2U, 2U, 0x1235U, 10000, 0x3AU) == 0U);
    for (uint8_t i = 0U; i < 5U; ++i)
    {
        app_current_calib_cal2_advance_ms(700U);
        gt_hsi_afe.curr_raw = 10000;
        gt_hsi_afe.current_update_seq++;
        app_current_calib_task();
    }
    assert(app_current_calib_cal2_request(3U, 3U, 0x1235U, 20000, 0x3AU) == 0U);
    s_fail_next_commit = 1U;
    for (uint8_t i = 0U; i < 5U; ++i)
    {
        app_current_calib_cal2_advance_ms(700U);
        gt_hsi_afe.curr_raw = 20000;
        gt_hsi_afe.current_update_seq++;
        app_current_calib_task();
    }
    assert(app_current_calib_cal2_request(5U, 4U, 0x1235U, 0, 0x3AU) == 7U);
    assert_shared_calibration(1000000U, -124);
    app_current_calib_init();
    assert_shared_calibration(1000000U, -124);

    /* Session timeout is fixed from BEGIN; point timeout is independently 15s. */
    gt_hsi_afe.curr_raw = 10000;
    gt_hsi_afe.current_update_seq++;
    app_current_calib_task();
    assert(app_current_calib_cal2_request(1U, 1U, 0x1236U, 0, 0x3AU) == 3U);
    assert(app_current_calib_cal2_request(2U, 2U, 0x1236U, 10000, 0x3AU) == 0U);
    app_current_calib_cal2_advance_ms(15000U);
    assert(app_current_calib_cal2_request(5U, 3U, 0x1236U, 0, 0x3AU) == 0x0CU);

    /* Crossing the 56-record page boundary must preserve the newest pair. */
    app_current_calib_init();
    for (uint8_t i = 0U; i < 60U; ++i)
    {
        sample_calibration_point(10000, 20000);
    }
    assert(s_cal2_page_b[0] == 'C' && s_cal2_page_b[1] == 'A');
    assert(app_current_calib_get(&k_ppm, &b_ma));
    app_current_calib_init();
    {
        uint32_t loaded_k;
        int32_t loaded_b;
        assert(app_current_calib_get(&loaded_k, &loaded_b));
        assert(loaded_k == k_ppm && loaded_b == b_ma);
    }
    return 0;
}
