#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "wifi.h"

#include "gdo.h"

#include "tasks.h"
#include "homekit_decl.h"
#include "homekit.h"
#include "app_health.h"
#include "app_events.h"
#include "app_settings.h"

static const char* TAG = "test_main";
static constexpr uint8_t MAX_ROLLING_CODE_RECOVERY_ATTEMPTS = 3;
static constexpr uint32_t ROLLING_CODE_RECOVERY_STEP = 100;
static constexpr uint32_t MAX_SECPLUS_V2_ROLLING_CODE = 0x0fffffff;
static uint8_t s_rolling_code_recovery_attempts;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static const char *gdo_event_code(gdo_cb_event_t event)
{
    switch (event) {
    case GDO_CB_EVENT_SYNCED:
        return "sync";
    case GDO_CB_EVENT_OBSTRUCTION:
        return "obstruction";
    case GDO_CB_EVENT_DOOR_POSITION:
        return "door_position";
    case GDO_CB_EVENT_LIGHT:
        return "light";
    case GDO_CB_EVENT_LOCK:
        return "lock";
    case GDO_CB_EVENT_MOTOR:
        return "motor";
    case GDO_CB_EVENT_BUTTON:
        return "button";
    case GDO_CB_EVENT_BATTERY:
        return "battery";
    case GDO_CB_EVENT_LEARN:
        return "learn";
    case GDO_CB_EVENT_OPENINGS:
        return "openings";
    case GDO_CB_EVENT_MOTION:
        return "motion";
    case GDO_CB_EVENT_TTC:
        return "time_to_close";
    case GDO_CB_EVENT_PAIRED_DEVICES:
        return "paired_devices";
    case GDO_CB_EVENT_OPEN_DURATION_MEASUREMENT:
        return "open_duration";
    case GDO_CB_EVENT_CLOSE_DURATION_MEASUREMENT:
        return "close_duration";
    default:
        return "unknown";
    }
}

static void log_gdo_event(const gdo_status_t *status, gdo_cb_event_t event)
{
    char data[96];
    const char *severity = "info";
    bool persist = false;

    switch (event) {
    case GDO_CB_EVENT_SYNCED:
        snprintf(data, sizeof(data), "{\"synced\":%s,\"protocol\":\"%s\"}",
                 status->synced ? "true" : "false", gdo_protocol_type_to_string(status->protocol));
        severity = status->synced ? "info" : "warn";
        persist = !status->synced;
        app_events_log("gdo", severity, status->synced ? "gdo_synced" : "gdo_sync_failed",
                       status->synced ? "GDO synchronized" : "GDO sync failed", data, persist);
        break;
    case GDO_CB_EVENT_DOOR_POSITION:
        snprintf(data, sizeof(data), "{\"door\":\"%s\",\"position\":%" PRId32 ",\"target\":%" PRId32 "}",
                 gdo_door_state_to_string(status->door), status->door_position, status->door_target);
        app_events_log("gdo", "info", "door_position", "Door position changed", data, false);
        break;
    case GDO_CB_EVENT_OBSTRUCTION:
        snprintf(data, sizeof(data), "{\"obstruction\":\"%s\"}", gdo_obstruction_state_to_string(status->obstruction));
        severity = status->obstruction == GDO_OBSTRUCTION_STATE_OBSTRUCTED ? "warn" : "info";
        app_events_log("gdo", severity, "obstruction", "Obstruction state changed", data,
                       status->obstruction == GDO_OBSTRUCTION_STATE_OBSTRUCTED);
        break;
    case GDO_CB_EVENT_MOTION:
        snprintf(data, sizeof(data), "{\"motion\":\"%s\"}", gdo_motion_state_to_string(status->motion));
        app_events_log("gdo", "info", "motion", "Motion state changed", data, false);
        break;
    case GDO_CB_EVENT_LIGHT:
        snprintf(data, sizeof(data), "{\"light\":\"%s\"}", gdo_light_state_to_string(status->light));
        app_events_log("gdo", "info", "light", "Light state changed", data, false);
        break;
    case GDO_CB_EVENT_LOCK:
        snprintf(data, sizeof(data), "{\"lock\":\"%s\"}", gdo_lock_state_to_string(status->lock));
        app_events_log("gdo", "info", "lock", "Lock state changed", data, false);
        break;
    case GDO_CB_EVENT_BATTERY:
        snprintf(data, sizeof(data), "{\"battery\":\"%s\"}", gdo_battery_state_to_string(status->battery));
        app_events_log("gdo", "info", "battery", "Battery state changed", data, false);
        break;
    case GDO_CB_EVENT_OPEN_DURATION_MEASUREMENT:
        snprintf(data, sizeof(data), "{\"open_ms\":%" PRIu16 "}", status->open_ms);
        app_events_log("gdo", "info", "open_duration", "Open duration measured", data, false);
        break;
    case GDO_CB_EVENT_CLOSE_DURATION_MEASUREMENT:
        snprintf(data, sizeof(data), "{\"close_ms\":%" PRIu16 "}", status->close_ms);
        app_events_log("gdo", "info", "close_duration", "Close duration measured", data, false);
        break;
    case GDO_CB_EVENT_PAIRED_DEVICES:
        snprintf(data, sizeof(data), "{\"total\":%u}", (unsigned)status->paired_devices.total_all);
        app_events_log("gdo", "info", "paired_devices", "Paired device counts changed", data, false);
        break;
    default:
        snprintf(data, sizeof(data), "{\"event\":\"%s\"}", gdo_event_code(event));
        app_events_log("gdo", "info", gdo_event_code(event), "GDO event received", data, false);
        break;
    }
}

static void gdo_event_handler(const gdo_status_t* status, gdo_cb_event_t event, void *arg)
{
    if (!status) {
        ESP_LOGW(TAG, "GDO callback received without status");
        return;
    }

    app_health_mark_gdo_event(status, event);
    log_gdo_event(status, event);

    switch (event) {
    case GDO_CB_EVENT_SYNCED:
        ESP_LOGI(TAG, "Synced: %s, protocol: %s", status->synced ? "true" : "false", gdo_protocol_type_to_string(status->protocol));
        if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {
            ESP_LOGI(TAG, "Client ID: %" PRIu32 ", Rolling code: %" PRIu32, status->client_id, status->rolling_code);
        }

        if (status->synced) {
            s_rolling_code_recovery_attempts = 0;
            if (status->protocol == GDO_PROTOCOL_SEC_PLUS_V2) {
                esp_err_t save_err = app_settings_save_secplus_identity(status->client_id, status->rolling_code);
                if (save_err != ESP_OK) {
                    ESP_LOGW(TAG, "Unable to persist Security+ identity: %s", esp_err_to_name(save_err));
                }
            }
        } else if (s_rolling_code_recovery_attempts < MAX_ROLLING_CODE_RECOVERY_ATTEMPTS) {
            const uint32_t old_rolling_code = status->rolling_code;
            if (old_rolling_code > MAX_SECPLUS_V2_ROLLING_CODE - ROLLING_CODE_RECOVERY_STEP) {
                ESP_LOGW(TAG, "GDO sync failed; rolling code is too close to the Security+ V2 limit to advance");
                break;
            }

            const uint32_t new_rolling_code = old_rolling_code + ROLLING_CODE_RECOVERY_STEP;
            if (gdo_set_rolling_code(new_rolling_code) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set rolling code");
            } else {
                ++s_rolling_code_recovery_attempts;
                ESP_LOGW(TAG,
                         "GDO sync failed; rolling code advanced from %" PRIu32 " to %" PRIu32
                         " for recovery attempt %u/%u",
                         old_rolling_code, new_rolling_code, (unsigned)s_rolling_code_recovery_attempts,
                         (unsigned)MAX_ROLLING_CODE_RECOVERY_ATTEMPTS);
                char data[96];
                snprintf(data, sizeof(data), "{\"attempt\":%u}", (unsigned)s_rolling_code_recovery_attempts);
                app_events_log("gdo", "warn", "rolling_code_recovery",
                               "Rolling code advanced for sync recovery", data, true);
                esp_err_t err = gdo_sync();
                if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "GDO recovery sync request failed: %s", esp_err_to_name(err));
                }
            }
        } else {
            ESP_LOGW(TAG, "GDO sync failed; leaving rolling code unchanged after %u recovery attempts",
                     (unsigned)s_rolling_code_recovery_attempts);
        }
        break;
    case GDO_CB_EVENT_LIGHT:
        ESP_LOGI(TAG, "Light: %s", gdo_light_state_to_string(status->light));
        notify_homekit_light(status->light);
        break;
    case GDO_CB_EVENT_LOCK:
        ESP_LOGI(TAG, "Lock: %s", gdo_lock_state_to_string(status->lock));
        notify_homekit_current_lock(status->lock);
        notify_homekit_target_lock(status->lock);
        break;
    case GDO_CB_EVENT_DOOR_POSITION:
        ESP_LOGI(TAG, "Door: %s, %.2f%%, target: %.2f%%", gdo_door_state_to_string(status->door),
                 (float)status->door_position, (float)status->door_target);
        notify_homekit_current_door_state_change(status->door);
        notify_homekit_target_door_state_change(status->door);
        break;
    case GDO_CB_EVENT_LEARN:
        ESP_LOGI(TAG, "Learn: %s", gdo_learn_state_to_string(status->learn));
        break;
    case GDO_CB_EVENT_OBSTRUCTION:
        ESP_LOGI(TAG, "Obstruction: %s", gdo_obstruction_state_to_string(status->obstruction));
        notify_homekit_obstruction(status->obstruction);
        break;
    case GDO_CB_EVENT_MOTION:
        ESP_LOGI(TAG, "Motion: %s", gdo_motion_state_to_string(status->motion));
        notify_homekit_motion(status->motion);
        break;
    case GDO_CB_EVENT_BATTERY:
        ESP_LOGI(TAG, "Battery: %s", gdo_battery_state_to_string(status->battery));
        notify_homekit_battery(status->battery);
        break;
    case GDO_CB_EVENT_BUTTON:
        ESP_LOGI(TAG, "Button: %s", gdo_button_state_to_string(status->button));
        notify_homekit_wall_button(status->button);
        break;
    case GDO_CB_EVENT_MOTOR:
        ESP_LOGI(TAG, "Motor: %s", gdo_motor_state_to_string(status->motor));
        break;
    case GDO_CB_EVENT_OPENINGS:
        ESP_LOGI(TAG, "Openings: %d", status->openings);
        break;
    case GDO_CB_EVENT_TTC:
        ESP_LOGI(TAG, "Time to close: %d", status->ttc_seconds);
        break;
    case GDO_CB_EVENT_PAIRED_DEVICES:
        ESP_LOGI(TAG, "Paired devices: %d remotes, %d keypads, %d wall controls, %d accessories, %d total",
                 status->paired_devices.total_remotes, status->paired_devices.total_keypads,
                 status->paired_devices.total_wall_controls, status->paired_devices.total_accessories,
                 status->paired_devices.total_all);
        break;
    default:
        ESP_LOGI(TAG, "Unknown event: %d", event);
        break;
    }
}

extern "C" void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(app_events_init());
    ESP_ERROR_CHECK(app_settings_init());

    const char *reset_reason = app_health_reset_reason_to_string(esp_reset_reason());
    char reset_data[48];
    snprintf(reset_data, sizeof(reset_data), "{\"reason\":\"%s\"}", reset_reason);
    app_events_logf("boot", "warn", "reset", true, reset_data, "Boot reset reason: %s", reset_reason);

    app_settings_t settings = {};
    app_settings_get(&settings);

    gdo_config_t gdo_conf;
    gdo_conf.invert_uart = true;
    gdo_conf.obst_from_status = settings.obstruction_source == APP_OBSTRUCTION_STATUS;
    gdo_conf.uart_num = UART_NUM_1;
    gdo_conf.uart_tx_pin = GPIO_NUM_1;
    gdo_conf.uart_rx_pin = GPIO_NUM_2;
    gdo_conf.obst_in_pin = GPIO_NUM_5;

    ESP_ERROR_CHECK(gdo_init(&gdo_conf));
    ESP_ERROR_CHECK(app_settings_apply_gdo_pre_start(&settings));
    ESP_ERROR_CHECK(gdo_start(gdo_event_handler, NULL));

    ESP_ERROR_CHECK(app_health_start());

    if (xTaskCreate(homekit_task_entry, HOMEKIT_TASK_NAME, HOMEKIT_TASK_STK_SZ, NULL, HOMEKIT_TASK_PRIO, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }

    ESP_LOGI(TAG, "GDO started!");
}
