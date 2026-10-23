#include <stdio.h>
#include <string.h>

#include "service_cfg.h"
#include "uds_service.h"
#include "SID10_SessionControl.h"
#include "SID22_ReadDataByIdentifier.h"
#include "SID27_SecurityAccess.h"
#include "uds_customer_profile.h"

#define RESPONSE_CAPACITY 256U

static uds_session_t mock_session = UDS_SESSION_STD;
static uint8_t last_response[RESPONSE_CAPACITY];
static uint16_t last_response_length;
static uint8_t last_negative_sid;
static uds_nrc_em last_nrc;
static int positive_response_result;
static int timer_locked;
static uint32_t mock_time_ms = 0U;
static unsigned int failures = 0U;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            (void)printf("FAIL: %s (line %d)\n", (message), __LINE__); \
            ++failures; \
        } \
    } while (0)

static void clear_transport_capture(void)
{
    (void)memset(last_response, 0, sizeof(last_response));
    last_response_length = 0U;
    last_negative_sid = 0U;
    last_nrc = NRC_GENERAL_REJECT;
}

static void check_response(const uint8_t *expected, uint16_t expected_length,
                           const char *message)
{
    CHECK(last_response_length == expected_length, message);
    if (last_response_length == expected_length)
    {
        CHECK(memcmp(last_response, expected, expected_length) == 0, message);
    }
}

static void send_seed(uint8_t subfunction, uint8_t seed_out[4])
{
    uint8_t request[2] = {SID_27, subfunction};

    clear_transport_capture();
    service_27_SecurityAccess(request, (uint16_t)sizeof(request));
    CHECK(last_response_length == 6U, "Seed response must contain four seed bytes.");
    CHECK(last_response[0] == 0x67U, "Seed response SID must be 0x67.");
    CHECK(last_response[1] == subfunction, "Seed response must echo the requested subfunction.");
    if (last_response_length == 6U)
    {
        (void)memcpy(seed_out, &last_response[2], 4U);
    }
}

static void send_key(uint8_t subfunction, const uint8_t key[4])
{
    uint8_t request[6] = {SID_27, subfunction, key[0], key[1], key[2], key[3]};

    clear_transport_capture();
    service_27_SecurityAccess(request, (uint16_t)sizeof(request));
}

static void calculate_key_for_seed(const uint8_t seed[4], uint8_t key[4])
{
    uds_customer_profile_calculate_key(seed, uds_customer_key_const, key);
}

/* Transport/session/timer seams: production UDS handlers are linked below. */
int uds_positive_rsp(uint8_t *data, uint16_t length)
{
    if (positive_response_result != 0)
    {
        return -1;
    }
    if ((data == NULL) || (length > RESPONSE_CAPACITY))
    {
        return -1;
    }
    (void)memcpy(last_response, data, length);
    last_response_length = length;
    return 0;
}

void uds_negative_rsp(uint8_t sid, uds_nrc_em nrc)
{
    last_negative_sid = sid;
    last_nrc = nrc;
    last_response_length = 0U;
}

uds_session_t get_current_session(void)
{
    return mock_session;
}

void set_current_session(uds_session_t session)
{
    mock_session = session;
}

int uds_timer_chk(uds_timer_t timer)
{
    (void)timer;
    return timer_locked;
}

void uds_timer_start(uds_timer_t timer)
{
    (void)timer;
    timer_locked = 1;
}

uint32_t d_timer_get_ms(void)
{
    return ++mock_time_ms;
}

const char *app_param_get_package_v_version(void)
{
    return "V01";
}

const char *app_param_get_package_t_version(void)
{
    return "T9";
}

const char *app_param_get_hw_version(void)
{
    return "V01";
}

static void test_version_did_bytes(void)
{
    static const uint8_t f189_expected[9] =
        {0x62U, 0xF1U, 0x89U, 'S', 'V', ':', 0x01U, '.', 0x09U};
    static const uint8_t f180_expected[9] =
        {0x62U, 0xF1U, 0x80U, 'B', 'V', ':', 0x00U, '.', 0x01U};
    static const uint8_t f193_expected[9] =
        {0x62U, 0xF1U, 0x93U, 'H', 'V', ':', 0x00U, '.', 0x01U};
    const uint8_t f189_request[3] = {SID_22, 0xF1U, 0x89U};
    const uint8_t f180_request[3] = {SID_22, 0xF1U, 0x80U};
    const uint8_t f193_request[3] = {SID_22, 0xF1U, 0x93U};

    clear_transport_capture();
    service_22_ReadDataByIdentifier(f189_request, (uint16_t)sizeof(f189_request));
    check_response(f189_expected, (uint16_t)sizeof(f189_expected),
                   "F189 must return the customer SV prefix and numeric version bytes.");

    clear_transport_capture();
    service_22_ReadDataByIdentifier(f180_request, (uint16_t)sizeof(f180_request));
    check_response(f180_expected, (uint16_t)sizeof(f180_expected),
                   "F180 must be fixed to BV:00.01.");

    clear_transport_capture();
    service_22_ReadDataByIdentifier(f193_request, (uint16_t)sizeof(f193_request));
    check_response(f193_expected, (uint16_t)sizeof(f193_expected),
                   "F193 must be fixed to HV:00.01, independent of app HW setting.");
}

static void test_level1_extended_session(void)
{
    uint8_t seed[4] = {0U, 0U, 0U, 0U};
    uint8_t key[4];
    uint8_t bad_order[6] = {SID_27, 0x02U, 0U, 0U, 0U, 0U};

    mock_session = UDS_SESSION_EXT;
    uds_security_reset();
    clear_transport_capture();
    service_27_SecurityAccess(bad_order, (uint16_t)sizeof(bad_order));
    CHECK(last_nrc == NRC_REQUEST_SEQUENCE_ERROR,
          "27 02 without a preceding 27 01 must be rejected.");
    CHECK(get_current_sa_lv() == UDS_SA_NON,
          "An out-of-sequence key must not unlock security.");

    send_seed(0x01U, seed);
    calculate_key_for_seed(seed, key);

    positive_response_result = -1;
    send_key(0x02U, key);
    CHECK(get_current_sa_lv() == UDS_SA_NON,
          "A valid key must not unlock if its positive response was not queued.");

    positive_response_result = 0;
    send_key(0x02U, key);
    CHECK(last_response_length == 2U && last_response[0] == 0x67U &&
          last_response[1] == 0x02U, "27 02 must positively respond after a valid key.");
    CHECK(get_current_sa_lv() == UDS_SA_LV1,
          "27 01/02 in Extended Session must unlock only level 1.");

    {
        uint8_t wrong_session_level[2] = {SID_27, 0x09U};
        clear_transport_capture();
        service_27_SecurityAccess(wrong_session_level,
                                 (uint16_t)sizeof(wrong_session_level));
        CHECK(last_nrc == NRC_SUBFUNCTION_NOT_SUPPORTED,
              "27 09 must not be accepted in Extended Session.");
    }
}

static void test_level2_programming_session_and_session_binding(void)
{
    uint8_t seed[4] = {0U, 0U, 0U, 0U};
    uint8_t key[4];

    mock_session = UDS_SESSION_EXT;
    uds_security_reset();
    send_seed(0x01U, seed);
    calculate_key_for_seed(seed, key);

    /* Simulate a session switch without the normal reset hook: level mismatch must still reject. */
    mock_session = UDS_SESSION_PROG;
    send_key(0x0AU, key);
    CHECK(last_nrc == NRC_REQUEST_SEQUENCE_ERROR,
          "A level-1 seed from Extended Session must not authorize 27 0A in Programming Session.");
    CHECK(get_current_sa_lv() == UDS_SA_NON,
          "A cross-session seed/key mismatch must leave security locked.");

    uds_security_reset();
    {
        uint8_t wrong_level_key[6] = {SID_27, 0x02U, 0U, 0U, 0U, 0U};
        clear_transport_capture();
        service_27_SecurityAccess(wrong_level_key,
                                 (uint16_t)sizeof(wrong_level_key));
        CHECK(last_nrc == NRC_SUBFUNCTION_NOT_SUPPORTED,
              "27 02 must not be accepted in Programming Session.");
    }

    send_seed(0x09U, seed);
    calculate_key_for_seed(seed, key);
    send_key(0x0AU, key);
    CHECK(last_response_length == 2U && last_response[0] == 0x67U &&
          last_response[1] == 0x0AU, "27 0A must positively respond after a valid key.");
    CHECK(get_current_sa_lv() == UDS_SA_LV2,
          "27 09/0A in Programming Session must unlock level 2.");

    uds_security_reset();
    CHECK(get_current_sa_lv() == UDS_SA_NON,
          "A session reset must clear the unlocked security level.");
}

static void test_unqueued_seed_is_not_accepted(void)
{
    uint8_t key_request[6] = {SID_27, 0x0AU, 0U, 0U, 0U, 0U};
    uint8_t seed_request[2] = {SID_27, 0x09U};

    mock_session = UDS_SESSION_PROG;
    uds_security_reset();
    positive_response_result = -1;
    clear_transport_capture();
    service_27_SecurityAccess(seed_request, (uint16_t)sizeof(seed_request));
    CHECK(last_response_length == 0U,
          "A seed response rejected by the transport must not be recorded as sent.");
    positive_response_result = 0;

    clear_transport_capture();
    service_27_SecurityAccess(key_request, (uint16_t)sizeof(key_request));
    CHECK(last_nrc == NRC_REQUEST_SEQUENCE_ERROR,
          "A seed whose response was not queued must not create a pending key exchange.");
    CHECK(get_current_sa_lv() == UDS_SA_NON,
          "An unqueued seed must not affect the security level.");
}

int main(void)
{
    positive_response_result = 0;
    timer_locked = 0;
    test_version_did_bytes();
    test_level1_extended_session();
    test_level2_programming_session_and_session_binding();
    test_unqueued_seed_is_not_accepted();

    if (failures != 0U)
    {
        (void)printf("UDS version/security behavior test: %u failure(s)\n", failures);
        return 1;
    }
    (void)printf("UDS version/security behavior test: PASS\n");
    return 0;
}
