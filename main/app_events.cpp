#include "app_events.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

static const char *TAG = "app_events";

static constexpr size_t RAM_EVENT_COUNT = 96;
static constexpr size_t CRITICAL_EVENT_COUNT = 16;
static constexpr uint32_t EVENTS_BLOB_MAGIC = 0x47444f45; // GDOE
static constexpr uint32_t EVENTS_BLOB_VERSION = 2;

typedef struct {
    uint32_t seq;
    uint32_t time_ms;
    int32_t value1;
    int32_t value2;
    int32_t value3;
    uint16_t code_id;
    uint8_t category_id;
    uint8_t severity_id;
} persisted_event_record_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t next;
    persisted_event_record_t records[CRITICAL_EVENT_COUNT];
} persisted_events_blob_t;

static_assert(sizeof(persisted_events_blob_t) <= 512, "Persisted event blob must stay small for NVS and stack");

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_persist_lock;
static persisted_event_record_t s_events[RAM_EVENT_COUNT];
static bool s_event_persisted[RAM_EVENT_COUNT];
static size_t s_event_count;
static size_t s_event_next;
static persisted_event_record_t s_critical[CRITICAL_EVENT_COUNT];
static size_t s_critical_count;
static size_t s_critical_next;
static uint32_t s_next_seq = 1;
static uint32_t s_dropped_events;
static bool s_initialized;

static uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = 0;
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static bool json_object_fragment(const char *value)
{
    if (!value) {
        return false;
    }
    size_t len = strlen(value);
    return len >= 2 && value[0] == '{' && value[len - 1] == '}';
}

typedef struct {
    uint16_t id;
    const char *name;
} id_name_t;

typedef struct {
    uint16_t id;
    const char *code;
    const char *message;
} code_meta_t;

enum : uint8_t {
    EVENT_CAT_APP = 0,
    EVENT_CAT_BOOT,
    EVENT_CAT_GDO,
    EVENT_CAT_HOMEKIT,
    EVENT_CAT_WIFI,
    EVENT_CAT_WATCHDOG,
    EVENT_CAT_HTTP,
    EVENT_CAT_SETTINGS,
};

enum : uint8_t {
    EVENT_SEV_INFO = 0,
    EVENT_SEV_WARN,
    EVENT_SEV_ERROR,
};

enum : uint16_t {
    EVENT_CODE_UNKNOWN = 0,
    EVENT_CODE_RESET,
    EVENT_CODE_GDO_SYNC_FAILED,
    EVENT_CODE_OBSTRUCTION,
    EVENT_CODE_ROLLING_CODE_RECOVERY,
    EVENT_CODE_ADMIN_PIN_SET,
    EVENT_CODE_EVENTS_CLEARED,
    EVENT_CODE_SETTINGS_SAVED,
    EVENT_CODE_CONTROLLER_PAIRED,
    EVENT_CODE_CONTROLLER_UNPAIRED,
    EVENT_CODE_PAIRING_ABORTED,
    EVENT_CODE_ACCESSORY_REBOOTING,
    EVENT_CODE_STA_DISCONNECTED,
    EVENT_CODE_WIFI_RECONNECT,
    EVENT_CODE_HEALTH_REBOOT,
    EVENT_CODE_HEALTH_RESYNC_FAILED,
    EVENT_CODE_HEALTH_STATUS_FAILED,
    EVENT_CODE_GDO_REFRESH,
    EVENT_CODE_MANUAL_SYNC,
    EVENT_CODE_PARTIAL_OPEN,
    EVENT_CODE_ADMIN_AUTH_FAILED,
    EVENT_CODE_PAIRING_STARTED,
    EVENT_CODE_DOOR_COMMAND,
    EVENT_CODE_LOCK_COMMAND,
    EVENT_CODE_LIGHT_COMMAND,
    EVENT_CODE_GDO_SYNCED,
    EVENT_CODE_DOOR_POSITION,
    EVENT_CODE_MOTION,
    EVENT_CODE_LIGHT,
    EVENT_CODE_LOCK,
    EVENT_CODE_BATTERY,
    EVENT_CODE_OPEN_DURATION,
    EVENT_CODE_CLOSE_DURATION,
    EVENT_CODE_PAIRED_DEVICES,
    EVENT_CODE_SYNC,
    EVENT_CODE_MOTOR,
    EVENT_CODE_BUTTON,
    EVENT_CODE_LEARN,
    EVENT_CODE_OPENINGS,
    EVENT_CODE_TIME_TO_CLOSE,
    EVENT_CODE_STA_GOT_IP,
    EVENT_CODE_AP_CLIENT_CONNECTED,
    EVENT_CODE_AP_CLIENT_DISCONNECTED,
    EVENT_CODE_PARTIAL_OPEN_REJECTED,
    EVENT_CODE_OTA_STARTED,
    EVENT_CODE_OTA_SUCCEEDED,
    EVENT_CODE_OTA_FAILED,
};

static const id_name_t CATEGORY_NAMES[] = {
    {EVENT_CAT_APP, "app"},
    {EVENT_CAT_BOOT, "boot"},
    {EVENT_CAT_GDO, "gdo"},
    {EVENT_CAT_HOMEKIT, "homekit"},
    {EVENT_CAT_WIFI, "wifi"},
    {EVENT_CAT_WATCHDOG, "watchdog"},
    {EVENT_CAT_HTTP, "http"},
    {EVENT_CAT_SETTINGS, "settings"},
};

static const id_name_t SEVERITY_NAMES[] = {
    {EVENT_SEV_INFO, "info"},
    {EVENT_SEV_WARN, "warn"},
    {EVENT_SEV_ERROR, "error"},
};

static const code_meta_t CODE_META[] = {
    {EVENT_CODE_UNKNOWN, "unknown", "Persisted critical event"},
    {EVENT_CODE_RESET, "reset", "Boot reset reason"},
    {EVENT_CODE_GDO_SYNC_FAILED, "gdo_sync_failed", "GDO sync failed"},
    {EVENT_CODE_OBSTRUCTION, "obstruction", "Obstruction state changed"},
    {EVENT_CODE_ROLLING_CODE_RECOVERY, "rolling_code_recovery", "Rolling code advanced for sync recovery"},
    {EVENT_CODE_ADMIN_PIN_SET, "admin_pin_set", "Admin PIN configured"},
    {EVENT_CODE_EVENTS_CLEARED, "events_cleared", "Diagnostics events cleared"},
    {EVENT_CODE_SETTINGS_SAVED, "settings_saved", "GDO settings saved"},
    {EVENT_CODE_CONTROLLER_PAIRED, "controller_paired", "HomeKit controller paired"},
    {EVENT_CODE_CONTROLLER_UNPAIRED, "controller_unpaired", "HomeKit controller unpaired"},
    {EVENT_CODE_PAIRING_ABORTED, "pairing_aborted", "HomeKit pairing aborted"},
    {EVENT_CODE_ACCESSORY_REBOOTING, "accessory_rebooting", "HomeKit accessory requested reboot"},
    {EVENT_CODE_STA_DISCONNECTED, "sta_disconnected", "STA Wi-Fi disconnected"},
    {EVENT_CODE_WIFI_RECONNECT, "wifi_reconnect", "STA has no IP; reconnect requested"},
    {EVENT_CODE_HEALTH_REBOOT, "health_reboot", "Health supervisor requested reboot"},
    {EVENT_CODE_HEALTH_RESYNC_FAILED, "health_resync_failed", "GDO health resync request failed"},
    {EVENT_CODE_HEALTH_STATUS_FAILED, "health_status_failed", "GDO health status request failed"},
    {EVENT_CODE_GDO_REFRESH, "gdo_refresh", "GDO status refresh failed"},
    {EVENT_CODE_MANUAL_SYNC, "manual_sync", "Manual GDO sync requested"},
    {EVENT_CODE_PARTIAL_OPEN, "partial_open", "Partial-open command failed"},
    {EVENT_CODE_ADMIN_AUTH_FAILED, "admin_auth_failed", "Admin authentication failed"},
    {EVENT_CODE_PAIRING_STARTED, "pairing_started", "HomeKit pairing started"},
    {EVENT_CODE_DOOR_COMMAND, "door_command", "HomeKit door target requested"},
    {EVENT_CODE_LOCK_COMMAND, "lock_command", "HomeKit lock target requested"},
    {EVENT_CODE_LIGHT_COMMAND, "light_command", "HomeKit light requested"},
    {EVENT_CODE_GDO_SYNCED, "gdo_synced", "GDO synchronized"},
    {EVENT_CODE_DOOR_POSITION, "door_position", "Door position changed"},
    {EVENT_CODE_MOTION, "motion", "Motion state changed"},
    {EVENT_CODE_LIGHT, "light", "Light state changed"},
    {EVENT_CODE_LOCK, "lock", "Lock state changed"},
    {EVENT_CODE_BATTERY, "battery", "Battery state changed"},
    {EVENT_CODE_OPEN_DURATION, "open_duration", "Open duration measured"},
    {EVENT_CODE_CLOSE_DURATION, "close_duration", "Close duration measured"},
    {EVENT_CODE_PAIRED_DEVICES, "paired_devices", "Paired device counts changed"},
    {EVENT_CODE_SYNC, "sync", "GDO sync event received"},
    {EVENT_CODE_MOTOR, "motor", "Motor state changed"},
    {EVENT_CODE_BUTTON, "button", "Button state changed"},
    {EVENT_CODE_LEARN, "learn", "Learn state changed"},
    {EVENT_CODE_OPENINGS, "openings", "Opening count changed"},
    {EVENT_CODE_TIME_TO_CLOSE, "time_to_close", "Time-to-close changed"},
    {EVENT_CODE_STA_GOT_IP, "sta_got_ip", "STA Wi-Fi got IP"},
    {EVENT_CODE_AP_CLIENT_CONNECTED, "ap_client_connected", "SoftAP client connected"},
    {EVENT_CODE_AP_CLIENT_DISCONNECTED, "ap_client_disconnected", "SoftAP client disconnected"},
    {EVENT_CODE_PARTIAL_OPEN_REJECTED, "partial_open_rejected", "Partial-open request rejected"},
    {EVENT_CODE_OTA_STARTED, "ota_started", "OTA update started"},
    {EVENT_CODE_OTA_SUCCEEDED, "ota_succeeded", "OTA update installed"},
    {EVENT_CODE_OTA_FAILED, "ota_failed", "OTA update failed"},
};

static const id_name_t REASON_NAMES[] = {
    {0, "unknown"},
    {1, "power-on"},
    {2, "external"},
    {3, "software"},
    {4, "panic"},
    {5, "interrupt-watchdog"},
    {6, "task-watchdog"},
    {7, "watchdog"},
    {8, "deep-sleep"},
    {9, "brownout"},
    {10, "sdio"},
    {20, "HomeKit did not start"},
    {21, "STA WiFi has been disconnected too long"},
    {22, "GDO has been unsynced too long"},
    {23, "GDO has not produced valid RX traffic too long"},
};

static const id_name_t RESULT_NAMES[] = {
    {0, "unknown"},
    {1, "ESP_OK"},
    {2, "ESP_FAIL"},
    {3, "ESP_ERR_NO_MEM"},
    {4, "ESP_ERR_INVALID_ARG"},
    {5, "ESP_ERR_INVALID_STATE"},
    {6, "ESP_ERR_TIMEOUT"},
    {7, "ESP_ERR_NOT_FINISHED"},
    {8, "ESP_ERR_NOT_FOUND"},
    {9, "ESP_ERR_NVS_NOT_ENOUGH_SPACE"},
    {10, "ESP_ERR_WIFI_NOT_INIT"},
};

static const id_name_t PROTOCOL_NAMES[] = {
    {0, "Unknown"},
    {1, "Security+ 1.0"},
    {2, "Security+ 2.0"},
    {3, "Security+ 1.0 with smart panel"},
};

static const id_name_t OBSTRUCTION_NAMES[] = {
    {0, "Unknown"},
    {1, "Obstructed"},
    {2, "Clear"},
};

static const id_name_t DOOR_NAMES[] = {
    {0, "Unknown"},
    {1, "Open"},
    {2, "Closed"},
    {3, "Stopped"},
    {4, "Opening"},
    {5, "Closing"},
};

static const id_name_t LIGHT_NAMES[] = {
    {0, "Off"},
    {1, "On"},
    {2, "Unknown"},
};

static const id_name_t LOCK_NAMES[] = {
    {0, "Unlocked"},
    {1, "Locked"},
    {2, "Unknown"},
};

static const id_name_t MOTION_NAMES[] = {
    {0, "Clear"},
    {1, "Detected"},
    {2, "Unknown"},
};

static const id_name_t BATTERY_NAMES[] = {
    {0, "Unknown"},
    {6, "Charging"},
    {8, "Full"},
};

static uint16_t id_for_name(const id_name_t *values, size_t count, const char *name, uint16_t fallback)
{
    if (!name) {
        return fallback;
    }
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(values[i].name, name) == 0) {
            return values[i].id;
        }
    }
    return fallback;
}

static const char *name_for_id(const id_name_t *values, size_t count, uint16_t id, const char *fallback)
{
    for (size_t i = 0; i < count; ++i) {
        if (values[i].id == id) {
            return values[i].name;
        }
    }
    return fallback;
}

static uint16_t code_id_for_name(const char *code)
{
    if (!code) {
        return EVENT_CODE_UNKNOWN;
    }
    for (size_t i = 0; i < sizeof(CODE_META) / sizeof(CODE_META[0]); ++i) {
        if (strcmp(CODE_META[i].code, code) == 0) {
            return CODE_META[i].id;
        }
    }
    return EVENT_CODE_UNKNOWN;
}

static const code_meta_t *code_meta_for_id(uint16_t id)
{
    for (size_t i = 0; i < sizeof(CODE_META) / sizeof(CODE_META[0]); ++i) {
        if (CODE_META[i].id == id) {
            return &CODE_META[i];
        }
    }
    return &CODE_META[0];
}

static int32_t json_int_value(const char *json, const char *key, int32_t fallback)
{
    if (!json || !key) {
        return fallback;
    }
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return fallback;
    }
    p += strlen(pattern);
    return (int32_t)strtol(p, NULL, 10);
}

static int32_t json_bool_value(const char *json, const char *key, int32_t fallback)
{
    if (!json || !key) {
        return fallback;
    }
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        return fallback;
    }
    p += strlen(pattern);
    if (strncmp(p, "true", 4) == 0) {
        return 1;
    }
    if (strncmp(p, "false", 5) == 0) {
        return 0;
    }
    return (int32_t)strtol(p, NULL, 10);
}

static bool json_string_value(const char *json, const char *key, char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) {
        return false;
    }
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        out[0] = 0;
        return false;
    }
    p += strlen(pattern);
    size_t len = strcspn(p, "\"");
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

static uint16_t json_string_id(const char *json, const char *key, const id_name_t *values,
                               size_t count, uint16_t fallback)
{
    char value[64];
    if (!json_string_value(json, key, value, sizeof(value))) {
        return fallback;
    }
    return id_for_name(values, count, value, fallback);
}

static void append_record_locked(const persisted_event_record_t *record, bool persisted)
{
    if (s_event_count == RAM_EVENT_COUNT) {
        ++s_dropped_events;
    } else {
        ++s_event_count;
    }
    s_events[s_event_next] = *record;
    s_event_persisted[s_event_next] = persisted;
    s_event_next = (s_event_next + 1) % RAM_EVENT_COUNT;

    if (record->seq >= s_next_seq) {
        s_next_seq = record->seq + 1;
    }
}

static void erase_critical_events_blob(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return;
    }
    err = nvs_erase_key(nvs, "crit_events");
    if (err == ESP_OK) {
        (void)nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static esp_err_t write_critical_events_blob(const persisted_events_blob_t *blob)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, "crit_events", blob, sizeof(*blob));
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        esp_err_t erase_err = nvs_erase_key(nvs, "crit_events");
        if (erase_err == ESP_ERR_NVS_NOT_FOUND) {
            erase_err = ESP_OK;
        }
        if (erase_err == ESP_OK) {
            erase_err = nvs_commit(nvs);
        }
        if (erase_err == ESP_OK) {
            err = nvs_set_blob(nvs, "crit_events", blob, sizeof(*blob));
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static persisted_event_record_t encode_persisted_record(const app_event_record_t *record)
{
    persisted_event_record_t out = {};
    if (!record) {
        return out;
    }

    out.seq = record->seq;
    out.time_ms = record->time_ms;
    out.category_id = (uint8_t)id_for_name(CATEGORY_NAMES, sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]),
                                           record->category, EVENT_CAT_APP);
    out.severity_id = (uint8_t)id_for_name(SEVERITY_NAMES, sizeof(SEVERITY_NAMES) / sizeof(SEVERITY_NAMES[0]),
                                           record->severity, EVENT_SEV_WARN);
    out.code_id = code_id_for_name(record->code);

    switch (out.code_id) {
    case EVENT_CODE_RESET:
    case EVENT_CODE_HEALTH_REBOOT:
        out.value1 = json_string_id(record->data_json, "reason", REASON_NAMES,
                                    sizeof(REASON_NAMES) / sizeof(REASON_NAMES[0]), 0);
        break;
    case EVENT_CODE_STA_DISCONNECTED:
        out.value1 = json_int_value(record->data_json, "reason", 0);
        break;
    case EVENT_CODE_GDO_SYNC_FAILED:
    case EVENT_CODE_GDO_SYNCED:
        out.value1 = json_string_id(record->data_json, "protocol", PROTOCOL_NAMES,
                                    sizeof(PROTOCOL_NAMES) / sizeof(PROTOCOL_NAMES[0]), 0);
        break;
    case EVENT_CODE_DOOR_POSITION:
        out.value1 = json_string_id(record->data_json, "door", DOOR_NAMES,
                                    sizeof(DOOR_NAMES) / sizeof(DOOR_NAMES[0]), 0);
        out.value2 = json_int_value(record->data_json, "position", -1);
        out.value3 = json_int_value(record->data_json, "target", -1);
        break;
    case EVENT_CODE_OBSTRUCTION:
        out.value1 = json_string_id(record->data_json, "obstruction", OBSTRUCTION_NAMES,
                                    sizeof(OBSTRUCTION_NAMES) / sizeof(OBSTRUCTION_NAMES[0]), 0);
        break;
    case EVENT_CODE_MOTION:
        out.value1 = json_string_id(record->data_json, "motion", MOTION_NAMES,
                                    sizeof(MOTION_NAMES) / sizeof(MOTION_NAMES[0]), 2);
        break;
    case EVENT_CODE_LIGHT:
        out.value1 = json_string_id(record->data_json, "light", LIGHT_NAMES,
                                    sizeof(LIGHT_NAMES) / sizeof(LIGHT_NAMES[0]), 2);
        break;
    case EVENT_CODE_LOCK:
        out.value1 = json_string_id(record->data_json, "lock", LOCK_NAMES,
                                    sizeof(LOCK_NAMES) / sizeof(LOCK_NAMES[0]), 2);
        break;
    case EVENT_CODE_BATTERY:
        out.value1 = json_string_id(record->data_json, "battery", BATTERY_NAMES,
                                    sizeof(BATTERY_NAMES) / sizeof(BATTERY_NAMES[0]), 0);
        break;
    case EVENT_CODE_DOOR_COMMAND:
    case EVENT_CODE_LOCK_COMMAND:
        out.value1 = json_int_value(record->data_json, "target", -1);
        break;
    case EVENT_CODE_LIGHT_COMMAND:
        out.value1 = json_bool_value(record->data_json, "on", 0);
        break;
    case EVENT_CODE_OPEN_DURATION:
        out.value1 = json_int_value(record->data_json, "open_ms", 0);
        break;
    case EVENT_CODE_CLOSE_DURATION:
        out.value1 = json_int_value(record->data_json, "close_ms", 0);
        break;
    case EVENT_CODE_PAIRED_DEVICES:
        out.value1 = json_int_value(record->data_json, "total", -1);
        break;
    case EVENT_CODE_ROLLING_CODE_RECOVERY:
        out.value1 = json_int_value(record->data_json, "attempt", 0);
        break;
    case EVENT_CODE_GDO_REFRESH:
    case EVENT_CODE_MANUAL_SYNC:
    case EVENT_CODE_HEALTH_RESYNC_FAILED:
    case EVENT_CODE_HEALTH_STATUS_FAILED:
        out.value1 = json_string_id(record->data_json, "result", RESULT_NAMES,
                                    sizeof(RESULT_NAMES) / sizeof(RESULT_NAMES[0]), 0);
        break;
    case EVENT_CODE_PARTIAL_OPEN:
        out.value1 = json_int_value(record->data_json, "target_percent", -1);
        out.value2 = json_string_id(record->data_json, "result", RESULT_NAMES,
                                    sizeof(RESULT_NAMES) / sizeof(RESULT_NAMES[0]), 0);
        break;
    default:
        break;
    }
    return out;
}

static void decode_compact_record(const persisted_event_record_t *record, bool persisted, app_event_record_t *out)
{
    if (!record || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    const code_meta_t *meta = code_meta_for_id(record->code_id);
    out->seq = record->seq;
    out->time_ms = record->time_ms;
    out->persisted = persisted;
    copy_text(out->category, sizeof(out->category),
              name_for_id(CATEGORY_NAMES, sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]),
                          record->category_id, "app"));
    copy_text(out->severity, sizeof(out->severity),
              name_for_id(SEVERITY_NAMES, sizeof(SEVERITY_NAMES) / sizeof(SEVERITY_NAMES[0]),
                          record->severity_id, "warn"));
    copy_text(out->code, sizeof(out->code), meta->code);
    copy_text(out->message, sizeof(out->message), meta->message);

    switch (record->code_id) {
    case EVENT_CODE_RESET: {
        const char *reason = name_for_id(REASON_NAMES, sizeof(REASON_NAMES) / sizeof(REASON_NAMES[0]),
                                         static_cast<uint16_t>(record->value1), "unknown");
        snprintf(out->message, sizeof(out->message), "Boot reset reason: %s", reason);
        snprintf(out->data_json, sizeof(out->data_json), "{\"reason\":\"%s\"}", reason);
        break;
    }
    case EVENT_CODE_HEALTH_REBOOT: {
        const char *reason = name_for_id(REASON_NAMES, sizeof(REASON_NAMES) / sizeof(REASON_NAMES[0]),
                                         static_cast<uint16_t>(record->value1), "unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"reason\":\"%s\"}", reason);
        break;
    }
    case EVENT_CODE_STA_DISCONNECTED:
        snprintf(out->data_json, sizeof(out->data_json), "{\"reason\":%" PRId32 "}", record->value1);
        break;
    case EVENT_CODE_GDO_SYNC_FAILED: {
        const char *protocol = name_for_id(PROTOCOL_NAMES, sizeof(PROTOCOL_NAMES) / sizeof(PROTOCOL_NAMES[0]),
                                           static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"synced\":false,\"protocol\":\"%s\"}", protocol);
        break;
    }
    case EVENT_CODE_GDO_SYNCED: {
        const char *protocol = name_for_id(PROTOCOL_NAMES, sizeof(PROTOCOL_NAMES) / sizeof(PROTOCOL_NAMES[0]),
                                           static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"synced\":true,\"protocol\":\"%s\"}", protocol);
        break;
    }
    case EVENT_CODE_DOOR_POSITION: {
        const char *door = name_for_id(DOOR_NAMES, sizeof(DOOR_NAMES) / sizeof(DOOR_NAMES[0]),
                                       static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json),
                 "{\"door\":\"%s\",\"position\":%" PRId32 ",\"target\":%" PRId32 "}",
                 door, record->value2, record->value3);
        break;
    }
    case EVENT_CODE_OBSTRUCTION: {
        const char *obstruction = name_for_id(OBSTRUCTION_NAMES, sizeof(OBSTRUCTION_NAMES) / sizeof(OBSTRUCTION_NAMES[0]),
                                              static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"obstruction\":\"%s\"}", obstruction);
        break;
    }
    case EVENT_CODE_MOTION: {
        const char *motion = name_for_id(MOTION_NAMES, sizeof(MOTION_NAMES) / sizeof(MOTION_NAMES[0]),
                                         static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"motion\":\"%s\"}", motion);
        break;
    }
    case EVENT_CODE_LIGHT: {
        const char *light = name_for_id(LIGHT_NAMES, sizeof(LIGHT_NAMES) / sizeof(LIGHT_NAMES[0]),
                                        static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"light\":\"%s\"}", light);
        break;
    }
    case EVENT_CODE_LOCK: {
        const char *lock = name_for_id(LOCK_NAMES, sizeof(LOCK_NAMES) / sizeof(LOCK_NAMES[0]),
                                       static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"lock\":\"%s\"}", lock);
        break;
    }
    case EVENT_CODE_BATTERY: {
        const char *battery = name_for_id(BATTERY_NAMES, sizeof(BATTERY_NAMES) / sizeof(BATTERY_NAMES[0]),
                                          static_cast<uint16_t>(record->value1), "Unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"battery\":\"%s\"}", battery);
        break;
    }
    case EVENT_CODE_DOOR_COMMAND:
    case EVENT_CODE_LOCK_COMMAND:
        snprintf(out->data_json, sizeof(out->data_json), "{\"target\":%" PRId32 "}", record->value1);
        break;
    case EVENT_CODE_LIGHT_COMMAND:
        snprintf(out->data_json, sizeof(out->data_json), "{\"on\":%s}", record->value1 ? "true" : "false");
        break;
    case EVENT_CODE_OPEN_DURATION:
        snprintf(out->data_json, sizeof(out->data_json), "{\"open_ms\":%" PRId32 "}", record->value1);
        break;
    case EVENT_CODE_CLOSE_DURATION:
        snprintf(out->data_json, sizeof(out->data_json), "{\"close_ms\":%" PRId32 "}", record->value1);
        break;
    case EVENT_CODE_PAIRED_DEVICES:
        snprintf(out->data_json, sizeof(out->data_json), "{\"total\":%" PRId32 "}", record->value1);
        break;
    case EVENT_CODE_ROLLING_CODE_RECOVERY:
        snprintf(out->data_json, sizeof(out->data_json),
                 "{\"attempt\":%" PRId32 "}", record->value3 ? record->value3 : record->value1);
        break;
    case EVENT_CODE_GDO_REFRESH:
    case EVENT_CODE_MANUAL_SYNC:
    case EVENT_CODE_HEALTH_RESYNC_FAILED:
    case EVENT_CODE_HEALTH_STATUS_FAILED: {
        const char *result = name_for_id(RESULT_NAMES, sizeof(RESULT_NAMES) / sizeof(RESULT_NAMES[0]),
                                         static_cast<uint16_t>(record->value1), "unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"result\":\"%s\"}", result);
        break;
    }
    case EVENT_CODE_PARTIAL_OPEN: {
        const char *result = name_for_id(RESULT_NAMES, sizeof(RESULT_NAMES) / sizeof(RESULT_NAMES[0]),
                                         static_cast<uint16_t>(record->value2), "unknown");
        snprintf(out->data_json, sizeof(out->data_json), "{\"target_percent\":%" PRId32 ",\"result\":\"%s\"}",
                 record->value1, result);
        break;
    }
    default:
        copy_text(out->data_json, sizeof(out->data_json), "{}");
        break;
    }
}

static void save_critical_events(void)
{
    persisted_events_blob_t blob = {};
    blob.magic = EVENTS_BLOB_MAGIC;
    blob.version = EVENTS_BLOB_VERSION;

    if (s_persist_lock) {
        xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        blob.count = (uint32_t)s_critical_count;
        blob.next = (uint32_t)s_critical_next;
        memcpy(blob.records, s_critical, sizeof(blob.records));
        xSemaphoreGive(s_lock);
    }

    esp_err_t err = write_critical_events_blob(&blob);
    if (s_persist_lock) {
        xSemaphoreGive(s_persist_lock);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to persist diagnostics events: %s", esp_err_to_name(err));
    }
}

static void add_critical_locked(const persisted_event_record_t *record)
{
    if (s_critical_count < CRITICAL_EVENT_COUNT) {
        ++s_critical_count;
    }
    s_critical[s_critical_next] = *record;
    s_critical_next = (s_critical_next + 1) % CRITICAL_EVENT_COUNT;
}

static void append_json_string(std::string &out, const char *value)
{
    out += '"';
    if (value) {
        for (const char *p = value; *p; ++p) {
            switch (*p) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if ((unsigned char)*p >= 0x20) {
                    out += *p;
                }
                break;
            }
        }
    }
    out += '"';
}

static void append_event_json(std::string &out, const app_event_record_t &event)
{
    out += "{\"seq\":";
    out += std::to_string(event.seq);
    out += ",\"time_ms\":";
    out += std::to_string(event.time_ms);
    out += ",\"category\":";
    append_json_string(out, event.category);
    out += ",\"severity\":";
    append_json_string(out, event.severity);
    out += ",\"code\":";
    append_json_string(out, event.code);
    out += ",\"message\":";
    append_json_string(out, event.message);
    out += ",\"data\":";
    out += json_object_fragment(event.data_json) ? event.data_json : "{}";
    out += ",\"persisted\":";
    out += event.persisted ? "true" : "false";
    out += '}';
}

esp_err_t app_events_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_persist_lock) {
        s_persist_lock = xSemaphoreCreateMutex();
        if (!s_persist_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    persisted_events_blob_t blob = {};
    size_t blob_size = sizeof(blob);
    nvs_handle_t nvs = 0;
    bool erase_stale_blob = false;
    esp_err_t err = nvs_open("gdo_diag", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_blob(nvs, "crit_events", &blob, &blob_size);
        nvs_close(nvs);
        if (err == ESP_OK && blob_size == sizeof(blob) && blob.magic == EVENTS_BLOB_MAGIC &&
            blob.version == EVENTS_BLOB_VERSION && blob.count <= CRITICAL_EVENT_COUNT &&
            blob.next < CRITICAL_EVENT_COUNT) {
            s_critical_count = blob.count;
            s_critical_next = blob.next;
            memcpy(s_critical, blob.records, sizeof(s_critical));
            for (size_t i = 0; i < s_critical_count; ++i) {
                size_t idx = (s_critical_next + CRITICAL_EVENT_COUNT - s_critical_count + i) % CRITICAL_EVENT_COUNT;
                append_record_locked(&s_critical[idx], true);
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            erase_stale_blob = true;
        }
    }

    s_initialized = true;
    xSemaphoreGive(s_lock);
    if (erase_stale_blob) {
        erase_critical_events_blob();
    }
    return ESP_OK;
}

void app_events_log(const char *category, const char *severity, const char *code,
                    const char *message, const char *data_json, bool persist)
{
    app_event_record_t record = {};
    record.time_ms = millis();
    copy_text(record.category, sizeof(record.category), category ? category : "app");
    copy_text(record.severity, sizeof(record.severity), severity ? severity : "info");
    copy_text(record.code, sizeof(record.code), code ? code : "event");
    copy_text(record.message, sizeof(record.message), message ? message : "");
    copy_text(record.data_json, sizeof(record.data_json), json_object_fragment(data_json) ? data_json : "{}");

    if (!s_lock && app_events_init() != ESP_OK) {
        ESP_LOGW(TAG, "Dropping event because diagnostics lock is unavailable");
        return;
    }

    const bool should_persist = persist;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    record.seq = s_next_seq++;
    persisted_event_record_t compact = encode_persisted_record(&record);
    if (should_persist) {
        record.persisted = true;
        add_critical_locked(&compact);
    }
    append_record_locked(&compact, should_persist);
    xSemaphoreGive(s_lock);

    if (should_persist) {
        save_critical_events();
    }
}

void app_events_logf(const char *category, const char *severity, const char *code,
                     bool persist, const char *data_json, const char *fmt, ...)
{
    char message[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    app_events_log(category, severity, code, message, data_json, persist);
}

esp_err_t app_events_clear(void)
{
    if (!s_lock && app_events_init() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_persist_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_events, 0, sizeof(s_events));
    memset(s_event_persisted, 0, sizeof(s_event_persisted));
    memset(s_critical, 0, sizeof(s_critical));
    s_event_count = 0;
    s_event_next = 0;
    s_critical_count = 0;
    s_critical_next = 0;
    s_dropped_events = 0;
    xSemaphoreGive(s_lock);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_diag", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(nvs, "crit_events");
        if (erase_err == ESP_ERR_NVS_NOT_FOUND) {
            erase_err = ESP_OK;
        }
        if (erase_err == ESP_OK) {
            erase_err = nvs_commit(nvs);
        }
        nvs_close(nvs);
        err = erase_err;
    }
    xSemaphoreGive(s_persist_lock);
    return err;
}

void app_events_get_summary(app_events_summary_t *summary)
{
    if (!summary) {
        return;
    }
    memset(summary, 0, sizeof(*summary));
    if (!s_lock && app_events_init() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    summary->total_events = (uint32_t)s_event_count;
    summary->dropped_events = s_dropped_events;
    summary->critical_events = (uint32_t)s_critical_count;
    if (s_critical_count > 0) {
        size_t idx = (s_critical_next + CRITICAL_EVENT_COUNT - 1) % CRITICAL_EVENT_COUNT;
        summary->has_last_critical = true;
        decode_compact_record(&s_critical[idx], true, &summary->last_critical);
    }
    xSemaphoreGive(s_lock);
}

std::string app_events_build_json(size_t limit, const char *category, const char *severity)
{
    if (limit == 0 || limit > RAM_EVENT_COUNT) {
        limit = RAM_EVENT_COUNT;
    }
    if (!s_lock && app_events_init() != ESP_OK) {
        return "{\"events\":[],\"error\":\"diagnostics unavailable\"}";
    }

    std::string out;
    out.reserve(2048 + (limit * 160));
    out += "{\"events\":[";
    const uint16_t category_filter = (category && category[0])
        ? id_for_name(CATEGORY_NAMES, sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]), category, UINT16_MAX)
        : UINT16_MAX;
    const uint16_t severity_filter = (severity && severity[0])
        ? id_for_name(SEVERITY_NAMES, sizeof(SEVERITY_NAMES) / sizeof(SEVERITY_NAMES[0]), severity, UINT16_MAX)
        : UINT16_MAX;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t emitted = 0;
    for (size_t i = 0; i < s_event_count && emitted < limit; ++i) {
        size_t idx = (s_event_next + RAM_EVENT_COUNT - 1 - i) % RAM_EVENT_COUNT;
        const persisted_event_record_t &compact = s_events[idx];
        if (category_filter != UINT16_MAX && compact.category_id != category_filter) {
            continue;
        }
        if (severity_filter != UINT16_MAX && compact.severity_id != severity_filter) {
            continue;
        }
        app_event_record_t event = {};
        decode_compact_record(&compact, s_event_persisted[idx], &event);
        if (emitted > 0) {
            out += ',';
        }
        append_event_json(out, event);
        ++emitted;
    }
    uint32_t total = (uint32_t)s_event_count;
    uint32_t dropped = s_dropped_events;
    uint32_t critical = (uint32_t)s_critical_count;
    xSemaphoreGive(s_lock);

    out += "],\"total_events\":";
    out += std::to_string(total);
    out += ",\"dropped_events\":";
    out += std::to_string(dropped);
    out += ",\"critical_events\":";
    out += std::to_string(critical);
    out += '}';
    return out;
}
