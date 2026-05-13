#pragma once

#include <stdint.h>
#include <string>

#include <esp_err.h>
#include <gdo.h>

typedef enum {
    APP_PROTOCOL_AUTO = 0,
    APP_PROTOCOL_SECPLUS_V1,
    APP_PROTOCOL_SECPLUS_V2,
    APP_PROTOCOL_SECPLUS_V1_PANEL,
} app_protocol_override_t;

typedef enum {
    APP_OBSTRUCTION_STATUS = 0,
    APP_OBSTRUCTION_GPIO,
} app_obstruction_source_t;

typedef struct {
    app_protocol_override_t protocol_override;
    app_obstruction_source_t obstruction_source;
    uint16_t open_ms;
    uint16_t close_ms;
    uint32_t min_command_interval_ms;
    bool toggle_only;
    bool secplus_identity_configured;
    uint32_t secplus_client_id;
    uint32_t secplus_rolling_code;
} app_settings_t;

typedef struct {
    bool protocol_requires_reboot;
    bool obstruction_requires_reboot;
    bool timing_requires_reboot;
} app_settings_apply_result_t;

esp_err_t app_settings_init(void);
void app_settings_get(app_settings_t *settings);
esp_err_t app_settings_save_and_apply(const app_settings_t *settings,
                                      const app_settings_t *previous,
                                      app_settings_apply_result_t *result);
esp_err_t app_settings_apply_gdo_runtime(const app_settings_t *settings);
esp_err_t app_settings_apply_gdo_pre_start(const app_settings_t *settings);
esp_err_t app_settings_save_secplus_identity(uint32_t client_id, uint32_t rolling_code);

const char *app_protocol_override_to_string(app_protocol_override_t value);
esp_err_t app_protocol_override_from_string(const char *value, app_protocol_override_t *out);
gdo_protocol_type_t app_protocol_override_to_gdo(app_protocol_override_t value);
const char *app_obstruction_source_to_string(app_obstruction_source_t value);
esp_err_t app_obstruction_source_from_string(const char *value, app_obstruction_source_t *out);

bool app_admin_pin_configured(void);
esp_err_t app_admin_set_pin(const char *pin);
bool app_admin_check_pin(const char *pin);
esp_err_t app_admin_validate_pin(const char *pin);

std::string app_settings_build_json(void);
