#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "app_events.h"
#include "app_settings.h"
#include "gdo_host.h"
#include "host_mocks.h"
#include "secplus.h"

extern "C" {
#include "esp_hap_pair_common.h"
}

static void reset_host(void)
{
    host_nvs_reset();
    host_time_set_ms(0);
    host_random_seed(0x1234abcd);
    host_gdo_reset();
}

static void test_secplus_roundtrips(void)
{
    uint8_t s1[20] = {};
    uint8_t s2[20] = {};
    uint32_t rolling32 = 0;
    uint32_t data32 = 0;

    assert(encode_v1(0x12345678, 123456, s1, s2) == 0);
    assert(decode_v1(s1, s2, &rolling32, &data32) == 0);
    assert(rolling32 == (0x12345678U & 0xfffffffeU));
    assert(data32 == 123456U);

    uint8_t p1[8] = {};
    uint8_t p2[8] = {};
    uint64_t fixed64 = 0;
    assert(encode_v2(0x01234567, 0x0123456789ULL, 0x00abcdef, 1, p1, p2) == 0);
    assert(decode_v2(1, p1, p2, &rolling32, &fixed64, &data32) == 0);
    assert(rolling32 == 0x01234567U);
    assert(fixed64 == 0x0123456789ULL);
    assert((data32 & 0xffff0fffU) == (0x00abcdefU & 0xffff0fffU));

    uint8_t wire[19] = {};
    assert(encode_wireline(0x01020304, 0x0102030405ULL, 0x000abcde, wire) == 0);
    assert(decode_wireline(wire, &rolling32, &fixed64, &data32) == 0);
    assert(rolling32 == 0x01020304U);
    assert(fixed64 == 0x0102030405ULL);
    assert((data32 & 0xffff0fffU) == (0x000abcdeU & 0xffff0fffU));
}

static void test_hap_tlv_helpers(void)
{
    uint8_t buf[640] = {};
    uint8_t value[300] = {};
    uint8_t out[300] = {};
    for (size_t i = 0; i < sizeof(value); ++i) {
        value[i] = static_cast<uint8_t>(i);
    }

    hap_tlv_data_t tlv = {};
    hap_tlv_data_init(&tlv, buf, sizeof(buf));
    assert(add_tlv(&tlv, kTLVType_State, sizeof(value), value) == 304);
    assert(get_tlv_length(buf, tlv.curlen, kTLVType_State) == 300);
    assert(get_value_from_tlv(buf, tlv.curlen, kTLVType_State, out, sizeof(out)) == 300);
    assert(memcmp(out, value, sizeof(value)) == 0);
    assert(get_tlv_length(buf, tlv.curlen - 1, kTLVType_State) == -1);
    assert(get_value_from_tlv(buf, tlv.curlen, kTLVType_State, out, 8) == -1);

    int outlen = 0;
    memset(buf, 0, sizeof(buf));
    hap_prepare_error_tlv(STATE_M2, kTLVError_Authentication, buf, sizeof(buf), &outlen);
    assert(outlen == 6);
    assert(get_tlv_length(buf, outlen, kTLVType_State) == 1);
    assert(get_tlv_length(buf, outlen, kTLVType_Error) == 1);

    uint8_t tight_buf[768] = {};
    uint8_t large_value[512] = {};
    hap_tlv_data_t tight = {};
    hap_tlv_data_init(&tight, tight_buf, sizeof(tight_buf));
    assert(add_tlv(&tight, kTLVType_State, sizeof(large_value), large_value) == 518);
    assert(add_tlv(&tight, kTLVType_State, 260, large_value) == -1);
    assert(tight.curlen == 518);
}

static void test_events(void)
{
    reset_host();
    assert(app_events_init() == ESP_OK);
    assert(app_events_clear() == ESP_OK);
    for (int i = 0; i < 100; ++i) {
        char data[48];
        snprintf(data, sizeof(data), "{\"reason\":%d}", i);
        app_events_log("wifi", i % 2 ? "warn" : "info", "sta_disconnected", "Wi-Fi drop", data, i < 20);
        host_time_advance_ms(10);
    }

    app_events_summary_t summary = {};
    app_events_get_summary(&summary);
    assert(summary.total_events == 96);
    assert(summary.dropped_events == 4);
    assert(summary.critical_events == 16);
    assert(summary.has_last_critical);
    assert(strcmp(summary.last_critical.category, "wifi") == 0);

    std::string json = app_events_build_json(5, "wifi", "warn");
    assert(json.find("\"events\"") != std::string::npos);
    assert(json.find("\"severity\":\"warn\"") != std::string::npos);
    assert(json.find("\"dropped_events\":4") != std::string::npos);
}

static void test_settings_and_admin_pin(void)
{
    reset_host();
    assert(app_settings_init() == ESP_OK);
    assert(app_admin_validate_pin("123") == ESP_ERR_INVALID_ARG);
    assert(app_admin_validate_pin("1234") == ESP_OK);
    assert(app_admin_set_pin("123456") == ESP_OK);
    assert(app_admin_pin_configured());
    assert(app_admin_check_pin("123456"));
    assert(!app_admin_check_pin("654321"));

    app_settings_t settings = {};
    app_settings_get(&settings);
    settings.protocol_override = APP_PROTOCOL_SECPLUS_V2;
    settings.obstruction_source = APP_OBSTRUCTION_GPIO;
    settings.open_ms = 11000;
    settings.close_ms = 12000;
    settings.min_command_interval_ms = 250;
    settings.toggle_only = true;

    app_settings_apply_result_t result = {};
    assert(app_settings_save_and_apply(&settings, nullptr, &result) == ESP_OK);

    gdo_status_t status = {};
    uint32_t last_rx_ms = 0;
    host_gdo_get_applied_settings(&status, &last_rx_ms);
    assert(status.open_ms == 11000);
    assert(status.close_ms == 12000);
    assert(host_gdo_get_min_command_interval_ms() == 250);
    assert(status.toggle_only);
    assert(last_rx_ms == 1000);

    assert(app_settings_save_secplus_identity(0x12345678, 0x01020304) == ESP_OK);
    app_settings_get(&settings);
    assert(settings.secplus_identity_configured);
    assert(settings.secplus_client_id == 0x12345678);
    assert(settings.secplus_rolling_code == 0x01020304);

    host_gdo_reset();
    assert(app_settings_apply_gdo_pre_start(&settings) == ESP_OK);
    host_gdo_get_applied_settings(&status, nullptr);
    assert(status.client_id == 0x12345678);
    assert(status.rolling_code == 0x01020304);

    std::string json = app_settings_build_json();
    assert(json.find("\"pin_configured\":true") != std::string::npos);
    assert(json.find("\"protocol\":\"Security+ 2.0\"") != std::string::npos);
    assert(json.find("\"secplus_identity_configured\":true") != std::string::npos);
    assert(json.find("\"secplus_client_id\":305419896") != std::string::npos);
}

int main(void)
{
    test_secplus_roundtrips();
    test_hap_tlv_helpers();
    test_events();
    test_settings_and_admin_pin();
    return 0;
}
