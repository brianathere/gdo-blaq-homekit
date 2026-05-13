#include "app_settings.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>
#include <sha/sha_core.h>

#include "app_events.h"

static const char *TAG = "app_settings";

static constexpr uint32_t DEFAULT_MIN_COMMAND_INTERVAL_MS = 50;
static constexpr uint32_t MAX_MIN_COMMAND_INTERVAL_MS = 60000;
static constexpr size_t ADMIN_SALT_SIZE = 16;
static constexpr size_t ADMIN_HASH_SIZE = 32;
static constexpr size_t ADMIN_PASSWORD_MAX_SIZE = 64;

static SemaphoreHandle_t s_lock;
static app_settings_t s_settings = {
    .protocol_override = APP_PROTOCOL_AUTO,
    .obstruction_source = APP_OBSTRUCTION_STATUS,
    .open_ms = 0,
    .close_ms = 0,
    .min_command_interval_ms = DEFAULT_MIN_COMMAND_INTERVAL_MS,
    .toggle_only = false,
};
static bool s_pin_configured;
static uint8_t s_pin_salt[ADMIN_SALT_SIZE];
static uint8_t s_pin_hash[ADMIN_HASH_SIZE];

static bool valid_pin_text(const char *pin)
{
    if (!pin) {
        return false;
    }
    size_t len = strlen(pin);
    if (len < 4 || len > ADMIN_PASSWORD_MAX_SIZE) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        if ((unsigned char)pin[i] < 0x21 || (unsigned char)pin[i] > 0x7e ||
            pin[i] == '"' || pin[i] == '\\' || pin[i] == ';' || pin[i] == ',') {
            return false;
        }
    }
    return true;
}

static bool valid_timing(uint16_t ms)
{
    return ms == 0 || (ms >= 1000 && ms <= 65000);
}

static void hash_pin(const char *pin, const uint8_t salt[ADMIN_SALT_SIZE], uint8_t out[ADMIN_HASH_SIZE])
{
    uint8_t input[ADMIN_SALT_SIZE + ADMIN_PASSWORD_MAX_SIZE] = {};
    size_t pin_len = strlen(pin);
    if (pin_len > ADMIN_PASSWORD_MAX_SIZE) {
        pin_len = ADMIN_PASSWORD_MAX_SIZE;
    }
    memcpy(input, salt, ADMIN_SALT_SIZE);
    memcpy(input + ADMIN_SALT_SIZE, pin, pin_len);
    esp_sha(SHA2_256, input, ADMIN_SALT_SIZE + pin_len, out);
}

static bool constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static esp_err_t save_settings_locked(const app_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_app", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(nvs, "protocol", settings->protocol_override);
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "obst_src", settings->obstruction_source);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "open_ms", settings->open_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "close_ms", settings->close_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "min_cmd_ms", settings->min_command_interval_ms);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "toggle_only", settings->toggle_only ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static esp_err_t load_settings(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_app", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }

    uint32_t value = 0;
    if (nvs_get_u32(nvs, "protocol", &value) == ESP_OK && value <= APP_PROTOCOL_SECPLUS_V1_PANEL) {
        s_settings.protocol_override = (app_protocol_override_t)value;
    }
    if (nvs_get_u32(nvs, "obst_src", &value) == ESP_OK && value <= APP_OBSTRUCTION_GPIO) {
        s_settings.obstruction_source = (app_obstruction_source_t)value;
    }
    if (nvs_get_u32(nvs, "open_ms", &value) == ESP_OK && value <= UINT16_MAX && valid_timing((uint16_t)value)) {
        s_settings.open_ms = (uint16_t)value;
    }
    if (nvs_get_u32(nvs, "close_ms", &value) == ESP_OK && value <= UINT16_MAX && valid_timing((uint16_t)value)) {
        s_settings.close_ms = (uint16_t)value;
    }
    if (nvs_get_u32(nvs, "min_cmd_ms", &value) == ESP_OK && value >= DEFAULT_MIN_COMMAND_INTERVAL_MS &&
        value <= MAX_MIN_COMMAND_INTERVAL_MS) {
        s_settings.min_command_interval_ms = value;
    }
    uint8_t toggle = 0;
    if (nvs_get_u8(nvs, "toggle_only", &toggle) == ESP_OK) {
        s_settings.toggle_only = toggle != 0;
    }

    size_t salt_len = sizeof(s_pin_salt);
    size_t hash_len = sizeof(s_pin_hash);
    if (nvs_get_blob(nvs, "pin_salt", s_pin_salt, &salt_len) == ESP_OK &&
        nvs_get_blob(nvs, "pin_hash", s_pin_hash, &hash_len) == ESP_OK &&
        salt_len == sizeof(s_pin_salt) && hash_len == sizeof(s_pin_hash)) {
        s_pin_configured = true;
    }

    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t app_settings_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = load_settings();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to load app settings: %s", esp_err_to_name(err));
    }
    return err;
}

void app_settings_get(app_settings_t *settings)
{
    if (!settings) {
        return;
    }
    if (!s_lock) {
        (void)app_settings_init();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *settings = s_settings;
    xSemaphoreGive(s_lock);
}

esp_err_t app_settings_apply_gdo_pre_start(const app_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }

    if (settings->protocol_override != APP_PROTOCOL_AUTO) {
        esp_err_t err = gdo_set_protocol(app_protocol_override_to_gdo(settings->protocol_override));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to apply protocol override: %s", esp_err_to_name(err));
            return err;
        }
    }
    return app_settings_apply_gdo_runtime(settings);
}

esp_err_t app_settings_apply_gdo_runtime(const app_settings_t *settings)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_timing(settings->open_ms) || !valid_timing(settings->close_ms) ||
        settings->min_command_interval_ms < DEFAULT_MIN_COMMAND_INTERVAL_MS ||
        settings->min_command_interval_ms > MAX_MIN_COMMAND_INTERVAL_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_err = ESP_OK;
    if (settings->open_ms > 0) {
        esp_err_t err = gdo_set_open_duration(settings->open_ms);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    if (settings->close_ms > 0) {
        esp_err_t err = gdo_set_close_duration(settings->close_ms);
        if (err != ESP_OK && first_err == ESP_OK) {
            first_err = err;
        }
    }
    esp_err_t err = gdo_set_min_command_interval(settings->min_command_interval_ms);
    if (err != ESP_OK && first_err == ESP_OK) {
        first_err = err;
    }
    gdo_set_toggle_only(settings->toggle_only);
    return first_err;
}

esp_err_t app_settings_save_and_apply(const app_settings_t *settings,
                                      const app_settings_t *previous,
                                      app_settings_apply_result_t *result)
{
    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_timing(settings->open_ms) || !valid_timing(settings->close_ms) ||
        settings->min_command_interval_ms < DEFAULT_MIN_COMMAND_INTERVAL_MS ||
        settings->min_command_interval_ms > MAX_MIN_COMMAND_INTERVAL_MS ||
        settings->protocol_override > APP_PROTOCOL_SECPLUS_V1_PANEL ||
        settings->obstruction_source > APP_OBSTRUCTION_GPIO) {
        return ESP_ERR_INVALID_ARG;
    }

    app_settings_t old_settings = {};
    if (previous) {
        old_settings = *previous;
    } else {
        app_settings_get(&old_settings);
    }

    app_settings_apply_result_t local_result = {};
    local_result.protocol_requires_reboot = settings->protocol_override != old_settings.protocol_override;
    local_result.obstruction_requires_reboot = settings->obstruction_source != old_settings.obstruction_source;
    local_result.timing_requires_reboot = (settings->open_ms == 0 && old_settings.open_ms != 0) ||
                                          (settings->close_ms == 0 && old_settings.close_ms != 0);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = save_settings_locked(settings);
    if (err == ESP_OK) {
        s_settings = *settings;
    }
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }

    esp_err_t apply_err = app_settings_apply_gdo_runtime(settings);
    if (apply_err != ESP_OK) {
        ESP_LOGW(TAG, "Settings saved but live GDO apply returned %s", esp_err_to_name(apply_err));
    }
    if (result) {
        *result = local_result;
    }
    return ESP_OK;
}

const char *app_protocol_override_to_string(app_protocol_override_t value)
{
    switch (value) {
    case APP_PROTOCOL_AUTO:
        return "auto";
    case APP_PROTOCOL_SECPLUS_V1:
        return "secplus_v1";
    case APP_PROTOCOL_SECPLUS_V2:
        return "secplus_v2";
    case APP_PROTOCOL_SECPLUS_V1_PANEL:
        return "secplus_v1_panel";
    default:
        return "unknown";
    }
}

esp_err_t app_protocol_override_from_string(const char *value, app_protocol_override_t *out)
{
    if (!value || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(value, "auto") == 0) {
        *out = APP_PROTOCOL_AUTO;
    } else if (strcmp(value, "secplus_v1") == 0) {
        *out = APP_PROTOCOL_SECPLUS_V1;
    } else if (strcmp(value, "secplus_v2") == 0) {
        *out = APP_PROTOCOL_SECPLUS_V2;
    } else if (strcmp(value, "secplus_v1_panel") == 0) {
        *out = APP_PROTOCOL_SECPLUS_V1_PANEL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

gdo_protocol_type_t app_protocol_override_to_gdo(app_protocol_override_t value)
{
    switch (value) {
    case APP_PROTOCOL_SECPLUS_V1:
        return GDO_PROTOCOL_SEC_PLUS_V1;
    case APP_PROTOCOL_SECPLUS_V2:
        return GDO_PROTOCOL_SEC_PLUS_V2;
    case APP_PROTOCOL_SECPLUS_V1_PANEL:
        return GDO_PROTOCOL_SEC_PLUS_V1_WITH_SMART_PANEL;
    case APP_PROTOCOL_AUTO:
    default:
        return GDO_PROTOCOL_MAX;
    }
}

const char *app_obstruction_source_to_string(app_obstruction_source_t value)
{
    switch (value) {
    case APP_OBSTRUCTION_STATUS:
        return "status";
    case APP_OBSTRUCTION_GPIO:
        return "gpio";
    default:
        return "unknown";
    }
}

esp_err_t app_obstruction_source_from_string(const char *value, app_obstruction_source_t *out)
{
    if (!value || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(value, "status") == 0) {
        *out = APP_OBSTRUCTION_STATUS;
    } else if (strcmp(value, "gpio") == 0) {
        *out = APP_OBSTRUCTION_GPIO;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bool app_admin_pin_configured(void)
{
    if (!s_lock) {
        (void)app_settings_init();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = s_pin_configured;
    xSemaphoreGive(s_lock);
    return configured;
}

esp_err_t app_admin_validate_pin(const char *pin)
{
    return valid_pin_text(pin) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t app_admin_set_pin(const char *pin)
{
    if (!valid_pin_text(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        esp_err_t init_err = app_settings_init();
        if (init_err != ESP_OK) {
            ESP_LOGW(TAG, "settings init failed: %s", esp_err_to_name(init_err));
            return init_err;
        }
    }

    uint8_t salt[ADMIN_SALT_SIZE];
    uint8_t hash[ADMIN_HASH_SIZE];
    esp_fill_random(salt, sizeof(salt));
    hash_pin(pin, salt, hash);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_app", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "pin_salt", salt, sizeof(salt));
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, "pin_hash", hash, sizeof(hash));
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_pin_salt, salt, sizeof(s_pin_salt));
    memcpy(s_pin_hash, hash, sizeof(s_pin_hash));
    s_pin_configured = true;
    xSemaphoreGive(s_lock);
    app_events_log("settings", "warn", "admin_pin_set", "Admin PIN configured", "{}", true);
    return ESP_OK;
}

bool app_admin_check_pin(const char *pin)
{
    if (!valid_pin_text(pin)) {
        return false;
    }
    if (!s_lock) {
        (void)app_settings_init();
    }

    uint8_t salt[ADMIN_SALT_SIZE];
    uint8_t expected[ADMIN_HASH_SIZE];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool configured = s_pin_configured;
    memcpy(salt, s_pin_salt, sizeof(salt));
    memcpy(expected, s_pin_hash, sizeof(expected));
    xSemaphoreGive(s_lock);
    if (!configured) {
        return false;
    }

    uint8_t actual[ADMIN_HASH_SIZE];
    hash_pin(pin, salt, actual);
    return constant_time_equal(actual, expected, sizeof(actual));
}

static void append_json_string(std::string &out, const char *value)
{
    out += '"';
    if (value) {
        for (const char *p = value; *p; ++p) {
            if (*p == '"' || *p == '\\') {
                out += '\\';
                out += *p;
            } else if ((unsigned char)*p >= 0x20) {
                out += *p;
            }
        }
    }
    out += '"';
}

std::string app_settings_build_json(void)
{
    app_settings_t settings = {};
    app_settings_get(&settings);

    gdo_status_t status = {};
    bool have_gdo = gdo_get_status(&status) == ESP_OK;
    uint32_t last_rx_ms = gdo_get_last_rx_ms();

    std::string out;
    out.reserve(1024);
    out += "{\"settings\":{";
    out += "\"protocol_override\":";
    append_json_string(out, app_protocol_override_to_string(settings.protocol_override));
    out += ",\"obstruction_source\":";
    append_json_string(out, app_obstruction_source_to_string(settings.obstruction_source));
    out += ",\"open_ms\":";
    out += std::to_string(settings.open_ms);
    out += ",\"close_ms\":";
    out += std::to_string(settings.close_ms);
    out += ",\"min_command_interval_ms\":";
    out += std::to_string(settings.min_command_interval_ms);
    out += ",\"toggle_only\":";
    out += settings.toggle_only ? "true" : "false";
    out += "},\"bounds\":{\"open_ms\":{\"min\":0,\"max\":65000},\"close_ms\":{\"min\":0,\"max\":65000},\"min_command_interval_ms\":{\"min\":50,\"max\":60000}},";
    out += "\"admin\":{\"pin_configured\":";
    out += app_admin_pin_configured() ? "true" : "false";
    out += "},\"diagnostics\":{";
    if (have_gdo) {
        out += "\"protocol\":";
        append_json_string(out, gdo_protocol_type_to_string(status.protocol));
        out += ",\"synced\":";
        out += status.synced ? "true" : "false";
        out += ",\"last_rx_ms\":";
        out += std::to_string(last_rx_ms);
    } else {
        out += "\"error\":\"gdo_get_status failed\"";
    }
    out += "}}";
    return out;
}
