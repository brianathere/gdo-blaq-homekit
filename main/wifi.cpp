#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

#include <esp_app_desc.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <gdo.h>
#include <json_parser.h>
#include <nvs_wifi_connect.h>

#include "app_events.h"
#include "app_health.h"
#include "app_settings.h"
#include "homekit.h"

extern "C" {
#include "qrcodegen.h"
}

static const char* TAG = "wifi";
static constexpr size_t ADMIN_PIN_BUFFER_SIZE = 72;
static constexpr size_t OTA_UPLOAD_BUFFER_SIZE = 4096;
static constexpr uint32_t OTA_REBOOT_DELAY_MS = 1200;

typedef struct {
    bool in_progress;
    bool has_last_result;
    bool last_success;
    uint32_t started_ms;
    uint32_t finished_ms;
    uint32_t expected_bytes;
    uint32_t written_bytes;
    esp_err_t last_error;
    char partition[17];
    char version[33];
} ota_runtime_state_t;

static ota_runtime_state_t s_ota_state = {};

static uint32_t millis(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *wifi_mode_to_string(wifi_mode_t mode) {
    switch (mode) {
    case WIFI_MODE_NULL:
        return "off";
    case WIFI_MODE_STA:
        return "sta";
    case WIFI_MODE_AP:
        return "ap";
    case WIFI_MODE_APSTA:
        return "apsta";
    default:
        return "unknown";
    }
}

static const char *format_mac(const uint8_t mac[6], char *buf, size_t size) {
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

static const char *format_ip(const esp_ip4_addr_t *ip, char *buf, size_t size) {
    snprintf(buf, size, IPSTR, IP2STR(ip));
    return buf;
}

static void json_escape(std::string &out, const char *value) {
    if (!value) {
        return;
    }

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

static void json_prop_prefix(std::string &out, bool &first, const char *name) {
    if (!first) {
        out += ',';
    }
    first = false;
    out += '"';
    json_escape(out, name);
    out += "\":";
}

static void json_string_value(std::string &out, const char *value) {
    out += '"';
    json_escape(out, value ? value : "");
    out += '"';
}

static void json_prop_string(std::string &out, bool &first, const char *name, const char *value) {
    json_prop_prefix(out, first, name);
    json_string_value(out, value);
}

static void json_prop_bool(std::string &out, bool &first, const char *name, bool value) {
    json_prop_prefix(out, first, name);
    out += value ? "true" : "false";
}

static void json_prop_null(std::string &out, bool &first, const char *name) {
    json_prop_prefix(out, first, name);
    out += "null";
}

static void json_prop_format(std::string &out, bool &first, const char *name, const char *fmt, ...) {
    char value[48];
    va_list args;
    va_start(args, fmt);
    vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);
    json_prop_prefix(out, first, name);
    out += value;
}

static void json_object_start(std::string &out, bool &first, const char *name, bool &object_first) {
    json_prop_prefix(out, first, name);
    out += '{';
    object_first = true;
}

static void add_u8_or_null(std::string &out, bool &first, const char *name, uint8_t value) {
    if (value == GDO_PAIRED_DEVICE_COUNT_UNKNOWN) {
        json_prop_null(out, first, name);
    } else {
        json_prop_format(out, first, name, "%u", value);
    }
}

static void add_position_or_null(std::string &out, bool &first, const char *name, int32_t value) {
    if (value < 0) {
        json_prop_null(out, first, name);
    } else {
        json_prop_format(out, first, name, "%.2f", value / 100.0);
    }
}

static void add_ip_info(std::string &out, bool &first, const char *name, const char *ifkey) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        return;
    }

    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return;
    }

    char addr[16];
    bool ip_first = true;
    json_object_start(out, first, name, ip_first);
    json_prop_string(out, ip_first, "address", format_ip(&ip.ip, addr, sizeof(addr)));
    json_prop_string(out, ip_first, "netmask", format_ip(&ip.netmask, addr, sizeof(addr)));
    json_prop_string(out, ip_first, "gateway", format_ip(&ip.gw, addr, sizeof(addr)));
    out += '}';
}

static const char *partition_subtype_to_string(esp_partition_type_t type, uint8_t subtype) {
    if (type == ESP_PARTITION_TYPE_APP) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_APP_FACTORY:
            return "factory";
        case ESP_PARTITION_SUBTYPE_APP_OTA_0:
            return "ota_0";
        case ESP_PARTITION_SUBTYPE_APP_OTA_1:
            return "ota_1";
        case ESP_PARTITION_SUBTYPE_APP_TEST:
            return "test";
        default:
            return "unknown";
        }
    }
    if (type == ESP_PARTITION_TYPE_DATA) {
        switch (subtype) {
        case ESP_PARTITION_SUBTYPE_DATA_OTA:
            return "otadata";
        case ESP_PARTITION_SUBTYPE_DATA_NVS:
            return "nvs";
        case ESP_PARTITION_SUBTYPE_DATA_PHY:
            return "phy";
        default:
            return "unknown";
        }
    }
    return "unknown";
}

static void json_partition_info(std::string &out, bool &first, const char *name, const esp_partition_t *partition) {
    if (!partition) {
        json_prop_null(out, first, name);
        return;
    }

    bool part_first = true;
    json_object_start(out, first, name, part_first);
    json_prop_string(out, part_first, "label", partition->label);
    json_prop_string(out, part_first, "type", partition->type == ESP_PARTITION_TYPE_APP ? "app" : "data");
    json_prop_string(out, part_first, "subtype", partition_subtype_to_string(partition->type, partition->subtype));
    json_prop_format(out, part_first, "address", "%" PRIu32, (uint32_t)partition->address);
    json_prop_format(out, part_first, "size", "%" PRIu32, (uint32_t)partition->size);
    out += '}';
}

static void ota_state_started(const esp_partition_t *partition, size_t expected_bytes) {
    s_ota_state.in_progress = true;
    s_ota_state.started_ms = millis();
    s_ota_state.finished_ms = 0;
    s_ota_state.expected_bytes = (uint32_t)expected_bytes;
    s_ota_state.written_bytes = 0;
    s_ota_state.last_error = ESP_OK;
    s_ota_state.last_success = false;
    s_ota_state.has_last_result = false;
    snprintf(s_ota_state.partition, sizeof(s_ota_state.partition), "%s", partition ? partition->label : "");
    s_ota_state.version[0] = 0;
}

static void ota_state_progress(size_t written_bytes) {
    s_ota_state.written_bytes = (uint32_t)written_bytes;
}

static void ota_state_finished(bool success, esp_err_t err, const char *version) {
    s_ota_state.in_progress = false;
    s_ota_state.has_last_result = true;
    s_ota_state.last_success = success;
    s_ota_state.last_error = err;
    s_ota_state.finished_ms = millis();
    if (version && version[0]) {
        snprintf(s_ota_state.version, sizeof(s_ota_state.version), "%s", version);
    }
}

static void append_ota_status_object(std::string &out, bool &root_first, const char *name) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);

    bool ota_first = true;
    json_object_start(out, root_first, name, ota_first);
    json_prop_bool(out, ota_first, "supported", next != nullptr);
    json_prop_bool(out, ota_first, "admin_pin_required", true);
    json_prop_bool(out, ota_first, "admin_pin_configured", app_admin_pin_configured());
    if (next) {
        json_prop_format(out, ota_first, "max_image_bytes", "%" PRIu32, (uint32_t)next->size);
    } else {
        json_prop_null(out, ota_first, "max_image_bytes");
    }
    json_partition_info(out, ota_first, "running_partition", running);
    json_partition_info(out, ota_first, "configured_boot_partition", configured);
    json_partition_info(out, ota_first, "next_update_partition", next);

    ota_runtime_state_t state = s_ota_state;
    bool runtime_first = true;
    json_object_start(out, ota_first, "runtime", runtime_first);
    json_prop_bool(out, runtime_first, "in_progress", state.in_progress);
    json_prop_format(out, runtime_first, "started_ms", "%" PRIu32, state.started_ms);
    json_prop_format(out, runtime_first, "finished_ms", "%" PRIu32, state.finished_ms);
    json_prop_format(out, runtime_first, "expected_bytes", "%" PRIu32, state.expected_bytes);
    json_prop_format(out, runtime_first, "written_bytes", "%" PRIu32, state.written_bytes);
    json_prop_string(out, runtime_first, "partition", state.partition);
    if (state.has_last_result) {
        json_prop_bool(out, runtime_first, "last_success", state.last_success);
        json_prop_string(out, runtime_first, "last_error", esp_err_to_name(state.last_error));
    } else {
        json_prop_null(out, runtime_first, "last_success");
        json_prop_null(out, runtime_first, "last_error");
    }
    if (state.version[0]) {
        json_prop_string(out, runtime_first, "last_version", state.version);
    } else {
        json_prop_null(out, runtime_first, "last_version");
    }
    out += '}';
    out += '}';
}

static std::string build_ota_status_json(void) {
    std::string out;
    out.reserve(1536);
    bool root_first = true;
    out += '{';
    append_ota_status_object(out, root_first, "ota");
    out += '}';
    return out;
}

static std::string build_status_json(void) {
    const uint32_t now = millis();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    std::string out;
    out.reserve(4096);
    bool root_first = true;

    out += '{';

    bool app_first = true;
    json_object_start(out, root_first, "app", app_first);
    app_health_snapshot_t health = {};
    app_health_get_snapshot(&health);
    json_prop_string(out, app_first, "project_name", app_desc ? app_desc->project_name : "");
    json_prop_string(out, app_first, "version", app_desc ? app_desc->version : "");
    json_prop_string(out, app_first, "idf_version", app_desc ? app_desc->idf_ver : "");
    json_prop_string(out, app_first, "build_date", app_desc ? app_desc->date : "");
    json_prop_string(out, app_first, "build_time", app_desc ? app_desc->time : "");
    json_prop_format(out, app_first, "uptime_ms", "%" PRIu32, now);
    json_prop_string(out, app_first, "reset_reason", app_health_reset_reason_to_string(esp_reset_reason()));
    json_prop_bool(out, app_first, "homekit_started", health.homekit_started);
    json_prop_format(out, app_first, "health_started_ms", "%" PRIu32, health.started_ms);
    json_prop_format(out, app_first, "last_gdo_event_ms", "%" PRIu32, health.last_gdo_event_ms);
    if (health.last_gdo_event_ms) {
        json_prop_format(out, app_first, "last_gdo_event_age_ms", "%" PRIu32, (uint32_t)(now - health.last_gdo_event_ms));
    } else {
        json_prop_null(out, app_first, "last_gdo_event_age_ms");
    }
    out += '}';

    append_ota_status_object(out, root_first, "ota");

    app_events_summary_t events_summary = {};
    app_events_get_summary(&events_summary);
    bool events_first = true;
    json_object_start(out, root_first, "events", events_first);
    json_prop_format(out, events_first, "total", "%" PRIu32, events_summary.total_events);
    json_prop_format(out, events_first, "critical", "%" PRIu32, events_summary.critical_events);
    json_prop_format(out, events_first, "dropped", "%" PRIu32, events_summary.dropped_events);
    if (events_summary.has_last_critical) {
        bool last_first = true;
        json_object_start(out, events_first, "last_critical", last_first);
        json_prop_format(out, last_first, "seq", "%" PRIu32, events_summary.last_critical.seq);
        json_prop_string(out, last_first, "category", events_summary.last_critical.category);
        json_prop_string(out, last_first, "severity", events_summary.last_critical.severity);
        json_prop_string(out, last_first, "code", events_summary.last_critical.code);
        json_prop_string(out, last_first, "message", events_summary.last_critical.message);
        out += '}';
    } else {
        json_prop_null(out, events_first, "last_critical");
    }
    out += '}';

    app_settings_t settings = {};
    app_settings_get(&settings);
    bool settings_first = true;
    json_object_start(out, root_first, "settings", settings_first);
    json_prop_string(out, settings_first, "protocol_override", app_protocol_override_to_string(settings.protocol_override));
    json_prop_string(out, settings_first, "obstruction_source", app_obstruction_source_to_string(settings.obstruction_source));
    json_prop_format(out, settings_first, "open_ms", "%u", settings.open_ms);
    json_prop_format(out, settings_first, "close_ms", "%u", settings.close_ms);
    json_prop_format(out, settings_first, "min_command_interval_ms", "%" PRIu32, settings.min_command_interval_ms);
    json_prop_bool(out, settings_first, "toggle_only", settings.toggle_only);
    json_prop_bool(out, settings_first, "admin_pin_configured", app_admin_pin_configured());
    out += '}';

    bool heap_first = true;
    json_object_start(out, root_first, "heap", heap_first);
    json_prop_format(out, heap_first, "free_bytes", "%" PRIu32, esp_get_free_heap_size());
    json_prop_format(out, heap_first, "minimum_free_bytes", "%" PRIu32, esp_get_minimum_free_heap_size());
    json_prop_format(out, heap_first, "internal_free_bytes", "%u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    json_prop_format(out, heap_first, "largest_free_block_bytes", "%u", heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    out += '}';

    bool wifi_first = true;
    json_object_start(out, root_first, "wifi", wifi_first);
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t mode_err = esp_wifi_get_mode(&mode);
    json_prop_string(out, wifi_first, "mode", mode_err == ESP_OK ? wifi_mode_to_string(mode) : "unknown");
    json_prop_format(out, wifi_first, "http_port", "%d", CONFIG_DEFAULT_NVS_WIFI_CONNECT_HTTP_PORT);

    uint8_t mac[6] = {0};
    char mac_str[18];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        json_prop_string(out, wifi_first, "sta_mac", format_mac(mac, mac_str, sizeof(mac_str)));
    }
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        json_prop_string(out, wifi_first, "ap_mac", format_mac(mac, mac_str, sizeof(mac_str)));
    }

    wifi_ap_record_t ap_info = {};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char ssid[sizeof(ap_info.ssid) + 1] = {};
        memcpy(ssid, ap_info.ssid, sizeof(ap_info.ssid));
        json_prop_bool(out, wifi_first, "sta_connected", true);
        json_prop_string(out, wifi_first, "ssid", ssid);
        json_prop_format(out, wifi_first, "rssi", "%d", ap_info.rssi);
        json_prop_format(out, wifi_first, "primary_channel", "%u", ap_info.primary);
        add_ip_info(out, wifi_first, "sta_ip", "WIFI_STA_DEF");
    } else {
        json_prop_bool(out, wifi_first, "sta_connected", false);
    }

    if (mode_err == ESP_OK && (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)) {
        wifi_sta_list_t sta_list = {};
        if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
            json_prop_format(out, wifi_first, "ap_connected_clients", "%u", sta_list.num);
        }
        add_ip_info(out, wifi_first, "ap_ip", "WIFI_AP_DEF");
    }
    out += '}';

    bool gdo_first = true;
    json_object_start(out, root_first, "gdo", gdo_first);
    gdo_status_t status = {};
    if (gdo_get_status(&status) == ESP_OK) {
        const uint32_t last_rx_ms = gdo_get_last_rx_ms();
        json_prop_bool(out, gdo_first, "synced", status.synced);
        json_prop_string(out, gdo_first, "protocol", gdo_protocol_type_to_string(status.protocol));
        json_prop_string(out, gdo_first, "door", gdo_door_state_to_string(status.door));
        json_prop_string(out, gdo_first, "light", gdo_light_state_to_string(status.light));
        json_prop_string(out, gdo_first, "lock", gdo_lock_state_to_string(status.lock));
        json_prop_string(out, gdo_first, "motion", gdo_motion_state_to_string(status.motion));
        json_prop_string(out, gdo_first, "obstruction", gdo_obstruction_state_to_string(status.obstruction));
        json_prop_string(out, gdo_first, "motor", gdo_motor_state_to_string(status.motor));
        json_prop_string(out, gdo_first, "button", gdo_button_state_to_string(status.button));
        json_prop_string(out, gdo_first, "battery", gdo_battery_state_to_string(status.battery));
        json_prop_string(out, gdo_first, "learn", gdo_learn_state_to_string(status.learn));
        json_prop_string(out, gdo_first, "last_move_direction", gdo_door_state_to_string(status.last_move_direction));
        json_prop_bool(out, gdo_first, "toggle_only", status.toggle_only);
        add_position_or_null(out, gdo_first, "door_position_percent", status.door_position);
        add_position_or_null(out, gdo_first, "door_target_percent", status.door_target);
        const bool door_moving = status.door == GDO_DOOR_STATE_OPENING || status.door == GDO_DOOR_STATE_CLOSING;
        const bool partial_open_available = status.synced && status.door_position >= 0 && status.open_ms > 0 &&
                                            status.close_ms > 0 && !door_moving &&
                                            status.obstruction != GDO_OBSTRUCTION_STATE_OBSTRUCTED;
        json_prop_bool(out, gdo_first, "partial_open_available", partial_open_available);
        if (!partial_open_available) {
            const char *reason = "unavailable";
            if (!status.synced) {
                reason = "not_synced";
            } else if (status.door_position < 0) {
                reason = "position_unknown";
            } else if (status.open_ms == 0 || status.close_ms == 0) {
                reason = "timing_unknown";
            } else if (door_moving) {
                reason = "door_moving";
            } else if (status.obstruction == GDO_OBSTRUCTION_STATE_OBSTRUCTED) {
                reason = "obstructed";
            }
            json_prop_string(out, gdo_first, "partial_open_unavailable_reason", reason);
        } else {
            json_prop_null(out, gdo_first, "partial_open_unavailable_reason");
        }
        json_prop_format(out, gdo_first, "openings", "%u", status.openings);
        json_prop_format(out, gdo_first, "ttc_seconds", "%u", status.ttc_seconds);
        json_prop_format(out, gdo_first, "open_ms", "%u", status.open_ms);
        json_prop_format(out, gdo_first, "close_ms", "%u", status.close_ms);
        json_prop_format(out, gdo_first, "last_rx_ms", "%" PRIu32, last_rx_ms);
        if (last_rx_ms) {
            json_prop_format(out, gdo_first, "last_rx_age_ms", "%" PRIu32, (uint32_t)(now - last_rx_ms));
        } else {
            json_prop_null(out, gdo_first, "last_rx_age_ms");
        }

        bool paired_first = true;
        json_object_start(out, gdo_first, "paired_devices", paired_first);
        add_u8_or_null(out, paired_first, "remotes", status.paired_devices.total_remotes);
        add_u8_or_null(out, paired_first, "keypads", status.paired_devices.total_keypads);
        add_u8_or_null(out, paired_first, "wall_controls", status.paired_devices.total_wall_controls);
        add_u8_or_null(out, paired_first, "accessories", status.paired_devices.total_accessories);
        add_u8_or_null(out, paired_first, "total", status.paired_devices.total_all);
        out += '}';
    } else {
        json_prop_string(out, gdo_first, "error", "gdo_get_status failed");
    }
    out += '}';

    bool homekit_first = true;
    json_object_start(out, root_first, "homekit", homekit_first);
    int paired_count = homekit_paired_controller_count();
    if (paired_count >= 0) {
        json_prop_format(out, homekit_first, "paired_controller_count", "%d", paired_count);
        json_prop_bool(out, homekit_first, "setup_available", paired_count == 0);
    } else {
        json_prop_null(out, homekit_first, "paired_controller_count");
        json_prop_bool(out, homekit_first, "setup_available", false);
    }
    json_prop_string(out, homekit_first, "model", "GDO blaQ HomeKit");
    json_prop_string(out, homekit_first, "hardware_revision", CONFIG_IDF_TARGET);
    json_prop_string(out, homekit_first, "firmware_revision", app_desc ? app_desc->version : "");
    out += '}';

    out += '}';
    return out;
}

static esp_err_t send_json_response(httpd_req_t *req, const std::string &json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json.c_str());
}

static esp_err_t send_error_response(httpd_req_t *req, const char *status, const char *error, const char *detail = nullptr) {
    httpd_resp_set_status(req, status);
    std::string json;
    json.reserve(128);
    json += "{\"ok\":false,\"error\":";
    json_string_value(json, error ? error : "error");
    if (detail) {
        json += ",\"detail\":";
        json_string_value(json, detail);
    }
    json += '}';
    return send_json_response(req, json);
}

static esp_err_t read_request_body(httpd_req_t *req, std::string &body, size_t max_len = 1024) {
    if (req->content_len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    body.clear();
    body.resize(req->content_len);
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret <= 0) {
            return ret == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        }
        received += (size_t)ret;
    }
    return ESP_OK;
}

static bool json_get_string_value(const std::string &body, const char *name, char *value, size_t value_size) {
    json_tok_t tokens[48];
    jparse_ctx_t ctx = {};
    if (json_parse_start_static(&ctx, body.c_str(), body.size(), tokens, sizeof(tokens) / sizeof(tokens[0])) != OS_SUCCESS) {
        return false;
    }
    bool ok = json_obj_get_string(&ctx, name, value, value_size) == OS_SUCCESS;
    json_parse_end_static(&ctx);
    return ok;
}

static bool json_get_int_value(const std::string &body, const char *name, int *value) {
    json_tok_t tokens[48];
    jparse_ctx_t ctx = {};
    if (json_parse_start_static(&ctx, body.c_str(), body.size(), tokens, sizeof(tokens) / sizeof(tokens[0])) != OS_SUCCESS) {
        return false;
    }
    bool ok = json_obj_get_int(&ctx, name, value) == OS_SUCCESS;
    json_parse_end_static(&ctx);
    return ok;
}

static bool json_get_bool_value(const std::string &body, const char *name, bool *value) {
    json_tok_t tokens[48];
    jparse_ctx_t ctx = {};
    if (json_parse_start_static(&ctx, body.c_str(), body.size(), tokens, sizeof(tokens) / sizeof(tokens[0])) != OS_SUCCESS) {
        return false;
    }
    bool ok = json_obj_get_bool(&ctx, name, value) == OS_SUCCESS;
    json_parse_end_static(&ctx);
    return ok;
}

static bool get_admin_pin_from_header(httpd_req_t *req, char *pin, size_t pin_size) {
    size_t len = httpd_req_get_hdr_value_len(req, "X-Admin-PIN");
    if (len == 0 || len >= pin_size) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, "X-Admin-PIN", pin, pin_size) == ESP_OK;
}

static bool get_admin_pin_from_cookie(httpd_req_t *req, char *pin, size_t pin_size) {
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len >= 256 || pin_size == 0) {
        return false;
    }

    char cookie[256];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    static const char *key = "gdo_admin_pin=";
    const size_t key_len = strlen(key);
    for (char *match = strstr(cookie, key); match; match = strstr(match + key_len, key)) {
        if (match != cookie && match[-1] != ';' && match[-1] != ' ') {
            continue;
        }
        const char *value = match + key_len;
        size_t value_len = strcspn(value, ";");
        if (value_len == 0 || value_len >= pin_size) {
            return false;
        }
        memcpy(pin, value, value_len);
        pin[value_len] = 0;
        return true;
    }
    return false;
}

static bool get_admin_pin_from_request(httpd_req_t *req, char *pin, size_t pin_size) {
    return get_admin_pin_from_header(req, pin, pin_size) ||
           get_admin_pin_from_cookie(req, pin, pin_size);
}

static bool get_admin_pin_from_body(const std::string &body, char *pin, size_t pin_size) {
    return json_get_string_value(body, "pin", pin, pin_size) ||
           json_get_string_value(body, "current_pin", pin, pin_size) ||
           json_get_string_value(body, "password", pin, pin_size);
}

static bool web_access_allowed(httpd_req_t *req) {
    if (!app_admin_pin_configured()) {
        return true;
    }

    char pin[ADMIN_PIN_BUFFER_SIZE] = {};
    if (get_admin_pin_from_request(req, pin, sizeof(pin)) && app_admin_check_pin(pin)) {
        return true;
    }

    (void)send_error_response(req, "401 Unauthorized", "web_auth_required");
    return false;
}

static esp_err_t require_web_access(httpd_req_t *req) {
    return web_access_allowed(req) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void set_admin_cookie(httpd_req_t *req, const char *pin) {
    if (!req || !pin || !pin[0]) {
        return;
    }
    char cookie[128];
    snprintf(cookie, sizeof(cookie), "gdo_admin_pin=%s; Path=/; SameSite=Strict; HttpOnly", pin);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static bool require_admin(httpd_req_t *req, const std::string *body = nullptr) {
    if (!app_admin_pin_configured()) {
        (void)send_error_response(req, "403 Forbidden", "admin_password_required");
        return false;
    }

    char pin[ADMIN_PIN_BUFFER_SIZE] = {};
    if (get_admin_pin_from_request(req, pin, sizeof(pin)) && app_admin_check_pin(pin)) {
        return true;
    }
    if (body && get_admin_pin_from_body(*body, pin, sizeof(pin)) && app_admin_check_pin(pin)) {
        return true;
    }

    app_events_log("http", "warn", "admin_auth_failed", "Admin authentication failed", "{}", false);
    (void)send_error_response(req, "403 Forbidden", "bad_admin_password");
    return false;
}

static esp_err_t require_admin_access(httpd_req_t *req) {
    return require_admin(req) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool query_value(httpd_req_t *req, const char *key, char *value, size_t value_size) {
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0 || query_len >= 128 || value_size == 0) {
        return false;
    }
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, key, value, value_size) == ESP_OK;
}

static size_t query_limit(httpd_req_t *req, size_t default_limit, size_t max_limit) {
    char value[12];
    if (!query_value(req, "limit", value, sizeof(value))) {
        return default_limit;
    }
    long parsed = strtol(value, nullptr, 10);
    if (parsed <= 0) {
        return default_limit;
    }
    if ((size_t)parsed > max_limit) {
        return max_limit;
    }
    return (size_t)parsed;
}

static esp_err_t access_get_handler(httpd_req_t *req) {
    const bool configured = app_admin_pin_configured();

    std::string json;
    json.reserve(96);
    json += "{\"password_configured\":";
    json += configured ? "true" : "false";
    json += ",\"admin_pin_configured\":";
    json += configured ? "true" : "false";
    json += ",\"authenticated\":";
    json += configured ? "false" : "true";
    json += '}';
    return send_json_response(req, json);
}

static esp_err_t status_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    return send_json_response(req, build_status_json());
}

static esp_err_t gdo_refresh_post_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    esp_err_t request_err = gdo_request_status();
    app_events_log("http", request_err == ESP_OK ? "info" : "warn", "gdo_refresh",
                   request_err == ESP_OK ? "GDO status refresh requested" : "GDO status refresh failed",
                   "{}", request_err != ESP_OK);
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"result\":\"%s\"}",
             request_err == ESP_OK ? "true" : "false", esp_err_to_name(request_err));
    if (request_err != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t events_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    char category[16] = {};
    char severity[8] = {};
    query_value(req, "category", category, sizeof(category));
    query_value(req, "severity", severity, sizeof(severity));
    size_t limit = query_limit(req, 50, 128);
    return send_json_response(req, app_events_build_json(limit, category, severity));
}

static const char OTA_PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>GDO Firmware Update</title>"
    "<style>"
    ":root{color-scheme:light dark;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#f6f7f9;color:#17202a}"
    "body{margin:0;padding:24px;display:flex;justify-content:center}"
    "main{width:min(760px,100%);display:grid;gap:16px}"
    "section{background:#fff;border:1px solid #d8dde6;border-radius:8px;padding:18px}"
    "h1{font-size:24px;margin:0 0 4px}h2{font-size:16px;margin:0 0 12px}"
    "label{display:grid;gap:6px;font-size:13px;color:#405066;margin:12px 0}"
    "input,button{font:inherit;border-radius:6px;border:1px solid #b8c0cc;padding:10px;background:#fff;color:inherit}"
    "button{background:#1264a3;color:#fff;border-color:#1264a3;cursor:pointer;min-height:42px}"
    "button:disabled{opacity:.55;cursor:not-allowed}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:12px}"
    ".kv{display:grid;grid-template-columns:160px 1fr;gap:8px;font-size:14px}.kv div{overflow-wrap:anywhere}"
    "pre{white-space:pre-wrap;word-break:break-word;background:#111827;color:#f9fafb;border-radius:8px;padding:12px;min-height:70px}"
    ".muted{color:#667085;font-size:13px}.ok{color:#067647}.bad{color:#b42318}"
    "@media (prefers-color-scheme:dark){:root{background:#111827;color:#f9fafb}section{background:#182230;border-color:#344054}input{background:#111827}.muted{color:#98a2b3}}"
    "@media (max-width:640px){body{padding:14px}.row{grid-template-columns:1fr}.kv{grid-template-columns:1fr}}"
    "</style></head><body><main>"
    "<header><h1>Firmware Update</h1><div class=\"muted\">GDO blaQ HomeKit on port 8080</div></header>"
    "<section><h2>Status</h2><div class=\"kv\">"
    "<b>Supported</b><div id=\"supported\">-</div>"
    "<b>Admin password</b><div id=\"admin\">-</div>"
    "<b>Running slot</b><div id=\"running\">-</div>"
    "<b>Next slot</b><div id=\"next\">-</div>"
    "<b>Max image</b><div id=\"max\">-</div>"
    "</div></section>"
    "<section><h2>Install</h2>"
    "<div class=\"row\"><label>Admin password<input id=\"pin\" type=\"password\" autocomplete=\"current-password\"></label>"
    "<label>Firmware binary<input id=\"file\" type=\"file\" accept=\".bin,application/octet-stream\"></label></div>"
    "<button id=\"upload\" type=\"button\">Install and Reboot</button>"
    "<p id=\"message\" class=\"muted\"></p></section>"
    "<section><h2>Response</h2><pre id=\"response\"></pre></section>"
    "</main><script>"
    "const $=id=>document.getElementById(id);"
    "function headers(){const p=$('pin').value;return p?{'X-Admin-PIN':p}:{};}"
    "function part(p){return p?p.label+' @ 0x'+Number(p.address).toString(16)+' ('+Math.round(p.size/1024)+' KB)':'-';}"
    "function msg(t,c){$('message').className=c||'muted';$('message').textContent=t;}"
    "async function refresh(){try{const r=await fetch('/api/ota',{headers:headers(),cache:'no-store'});const t=await r.text();$('response').textContent=t;const j=JSON.parse(t).ota;"
    "$('supported').textContent=j.supported?'yes':'no';$('supported').className=j.supported?'ok':'bad';"
    "$('admin').textContent=j.admin_pin_configured?'configured':'required before OTA';$('admin').className=j.admin_pin_configured?'ok':'bad';"
    "$('running').textContent=part(j.running_partition);$('next').textContent=part(j.next_update_partition);"
    "$('max').textContent=j.max_image_bytes?Math.round(j.max_image_bytes/1024)+' KB':'-';"
    "$('upload').disabled=!j.supported||!j.admin_pin_configured||j.runtime.in_progress;}catch(e){msg('Status unavailable: '+e,'bad');}}"
    "$('upload').onclick=async()=>{const f=$('file').files[0];if(!f){msg('Select a firmware .bin first.','bad');return;}if(!$('pin').value){msg('Enter the admin password.','bad');return;}"
    "msg('Uploading '+f.name+' ('+f.size+' bytes)...');$('upload').disabled=true;"
    "try{const h=Object.assign({'Content-Type':'application/octet-stream'},headers());const r=await fetch('/api/ota',{method:'POST',headers:h,body:f});const t=await r.text();$('response').textContent=t;"
    "if(!r.ok){msg('Update failed: '+t,'bad');$('upload').disabled=false;return;}msg('Update installed. Device is rebooting.','ok');setTimeout(refresh,8000);}"
    "catch(e){msg('Upload failed: '+e,'bad');$('upload').disabled=false;}};"
    "$('pin').onchange=refresh;refresh();"
    "</script></body></html>";

static esp_err_t ota_page_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, OTA_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    return send_json_response(req, build_ota_status_json());
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    if (!require_admin(req)) {
        return ESP_OK;
    }
    if (s_ota_state.in_progress) {
        return send_error_response(req, "409 Conflict", "ota_in_progress");
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(nullptr);
    if (!update_partition) {
        return send_error_response(req, "409 Conflict", "ota_not_available", "partition table has no OTA slot");
    }
    if (req->content_len == 0) {
        return send_error_response(req, "400 Bad Request", "empty_firmware");
    }
    if (req->content_len > update_partition->size) {
        return send_error_response(req, "413 Payload Too Large", "firmware_too_large");
    }

    ota_state_started(update_partition, req->content_len);
    char event_data[128];
    snprintf(event_data, sizeof(event_data), "{\"partition\":\"%s\",\"bytes\":%u}",
             update_partition->label, (unsigned)req->content_len);
    app_events_log("http", "warn", "ota_started", "OTA update started", event_data, true);

    uint8_t *buffer = static_cast<uint8_t *>(malloc(OTA_UPLOAD_BUFFER_SIZE));
    if (!buffer) {
        ota_state_finished(false, ESP_ERR_NO_MEM, nullptr);
        app_events_log("http", "error", "ota_failed", "OTA update failed", "{\"result\":\"ESP_ERR_NO_MEM\"}", true);
        return send_error_response(req, "500 Internal Server Error", "out_of_memory");
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota_handle);
    bool handle_open = err == ESP_OK;
    size_t remaining = req->content_len;
    size_t written = 0;
    int timeouts = 0;

    while (err == ESP_OK && remaining > 0) {
        size_t to_read = remaining < OTA_UPLOAD_BUFFER_SIZE ? remaining : OTA_UPLOAD_BUFFER_SIZE;
        int received = httpd_req_recv(req, reinterpret_cast<char *>(buffer), to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 20) {
                err = ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(ota_handle, buffer, (size_t)received);
        if (err != ESP_OK) {
            break;
        }
        written += (size_t)received;
        remaining -= (size_t)received;
        ota_state_progress(written);
    }

    free(buffer);

    if (err == ESP_OK) {
        err = esp_ota_end(ota_handle);
        handle_open = false;
    }
    if (err != ESP_OK) {
        if (handle_open) {
            (void)esp_ota_abort(ota_handle);
        }
        snprintf(event_data, sizeof(event_data), "{\"result\":\"%s\",\"written\":%u}",
                 esp_err_to_name(err), (unsigned)written);
        ota_state_finished(false, err, nullptr);
        app_events_log("http", "error", "ota_failed", "OTA update failed", event_data, true);
        return send_error_response(req, "400 Bad Request", "ota_write_failed", esp_err_to_name(err));
    }

    esp_app_desc_t new_app = {};
    const char *new_version = "";
    if (esp_ota_get_partition_description(update_partition, &new_app) == ESP_OK) {
        new_version = new_app.version;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        snprintf(event_data, sizeof(event_data), "{\"result\":\"%s\",\"partition\":\"%s\"}",
                 esp_err_to_name(err), update_partition->label);
        ota_state_finished(false, err, new_version);
        app_events_log("http", "error", "ota_failed", "OTA update failed", event_data, true);
        return send_error_response(req, "500 Internal Server Error", "ota_boot_partition_failed", esp_err_to_name(err));
    }

    ota_state_finished(true, ESP_OK, new_version);
    snprintf(event_data, sizeof(event_data), "{\"partition\":\"%s\",\"bytes\":%u}",
             update_partition->label, (unsigned)written);
    app_events_log("http", "warn", "ota_succeeded", "OTA update installed", event_data, true);

    std::string json;
    json.reserve(192);
    json += "{\"ok\":true,\"bytes\":";
    json += std::to_string(written);
    json += ",\"partition\":";
    json_string_value(json, update_partition->label);
    json += ",\"version\":";
    json_string_value(json, new_version);
    json += ",\"reboot_ms\":";
    json += std::to_string(OTA_REBOOT_DELAY_MS);
    json += '}';
    esp_err_t send_err = send_json_response(req, json);
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
    return send_err;
}

static esp_err_t events_clear_post_handler(httpd_req_t *req) {
    if (!require_admin(req)) {
        return ESP_OK;
    }
    esp_err_t err = app_events_clear();
    if (err != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "clear_failed", esp_err_to_name(err));
    }
    app_events_log("settings", "warn", "events_cleared", "Diagnostics events cleared", "{}", true);
    return send_json_response(req, "{\"ok\":true}");
}

static esp_err_t admin_setup_post_handler(httpd_req_t *req) {
    std::string body;
    esp_err_t err = read_request_body(req, body, 256);
    if (err != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "bad_request", esp_err_to_name(err));
    }

    const bool configured = app_admin_pin_configured();
    if (configured) {
        if (!require_admin(req, &body)) {
            return ESP_OK;
        }
    }

    char pin[ADMIN_PIN_BUFFER_SIZE] = {};
    if (configured) {
        if (!json_get_string_value(body, "new_pin", pin, sizeof(pin)) &&
            !json_get_string_value(body, "new_password", pin, sizeof(pin)) &&
            !json_get_string_value(body, "pin", pin, sizeof(pin))) {
            return send_error_response(req, "400 Bad Request", "missing_new_password");
        }
    } else if (!json_get_string_value(body, "pin", pin, sizeof(pin)) &&
               !json_get_string_value(body, "password", pin, sizeof(pin))) {
        return send_error_response(req, "400 Bad Request", "missing_password");
    }

    err = app_admin_set_pin(pin);
    if (err != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "invalid_password", esp_err_to_name(err));
    }
    set_admin_cookie(req, pin);
    return send_json_response(req, "{\"ok\":true,\"password_configured\":true}");
}

static esp_err_t admin_check_post_handler(httpd_req_t *req) {
    std::string body;
    esp_err_t err = read_request_body(req, body, 256);
    if (err != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "bad_request", esp_err_to_name(err));
    }
    if (!require_admin(req, &body)) {
        return ESP_OK;
    }
    char pin[ADMIN_PIN_BUFFER_SIZE] = {};
    if (get_admin_pin_from_request(req, pin, sizeof(pin)) ||
        get_admin_pin_from_body(body, pin, sizeof(pin))) {
        set_admin_cookie(req, pin);
    }
    return send_json_response(req, "{\"ok\":true,\"authenticated\":true}");
}

static esp_err_t settings_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    return send_json_response(req, app_settings_build_json());
}

static esp_err_t settings_post_handler(httpd_req_t *req) {
    std::string body;
    esp_err_t err = read_request_body(req, body, 1024);
    if (err != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "bad_request", esp_err_to_name(err));
    }
    if (!require_admin(req, &body)) {
        return ESP_OK;
    }

    app_settings_t old_settings = {};
    app_settings_get(&old_settings);
    app_settings_t settings = old_settings;

    char text_value[32] = {};
    int int_value = 0;
    bool bool_value = false;

    if (json_get_string_value(body, "protocol_override", text_value, sizeof(text_value))) {
        app_protocol_override_t parsed = APP_PROTOCOL_AUTO;
        if (app_protocol_override_from_string(text_value, &parsed) != ESP_OK) {
            return send_error_response(req, "400 Bad Request", "invalid_protocol_override");
        }
        settings.protocol_override = parsed;
    }
    if (json_get_string_value(body, "obstruction_source", text_value, sizeof(text_value))) {
        app_obstruction_source_t parsed = APP_OBSTRUCTION_STATUS;
        if (app_obstruction_source_from_string(text_value, &parsed) != ESP_OK) {
            return send_error_response(req, "400 Bad Request", "invalid_obstruction_source");
        }
        settings.obstruction_source = parsed;
    }
    if (json_get_int_value(body, "open_ms", &int_value)) {
        if (int_value < 0 || int_value > 65000 || (int_value != 0 && int_value < 1000)) {
            return send_error_response(req, "400 Bad Request", "invalid_open_ms");
        }
        settings.open_ms = (uint16_t)int_value;
    }
    if (json_get_int_value(body, "close_ms", &int_value)) {
        if (int_value < 0 || int_value > 65000 || (int_value != 0 && int_value < 1000)) {
            return send_error_response(req, "400 Bad Request", "invalid_close_ms");
        }
        settings.close_ms = (uint16_t)int_value;
    }
    if (json_get_int_value(body, "min_command_interval_ms", &int_value)) {
        if (int_value < 50 || int_value > 60000) {
            return send_error_response(req, "400 Bad Request", "invalid_min_command_interval_ms");
        }
        settings.min_command_interval_ms = (uint32_t)int_value;
    }
    if (json_get_bool_value(body, "toggle_only", &bool_value)) {
        settings.toggle_only = bool_value;
    }

    app_settings_apply_result_t result = {};
    err = app_settings_save_and_apply(&settings, &old_settings, &result);
    if (err != ESP_OK) {
        return send_error_response(req, "500 Internal Server Error", "settings_save_failed", esp_err_to_name(err));
    }

    app_events_log("settings", "warn", "settings_saved", "GDO settings saved", "{}", true);
    std::string json;
    json.reserve(256);
    json += "{\"ok\":true,\"requires_reboot\":";
    json += (result.protocol_requires_reboot || result.obstruction_requires_reboot || result.timing_requires_reboot) ? "true" : "false";
    json += ",\"reasons\":{\"protocol\":";
    json += result.protocol_requires_reboot ? "true" : "false";
    json += ",\"obstruction_source\":";
    json += result.obstruction_requires_reboot ? "true" : "false";
    json += ",\"timing\":";
    json += result.timing_requires_reboot ? "true" : "false";
    json += "}}";
    return send_json_response(req, json);
}

static esp_err_t gdo_sync_post_handler(httpd_req_t *req) {
    if (!require_admin(req)) {
        return ESP_OK;
    }
    esp_err_t err = gdo_sync();
    char data[48];
    snprintf(data, sizeof(data), "{\"result\":\"%s\"}", esp_err_to_name(err));
    app_events_log("gdo", err == ESP_OK || err == ESP_ERR_NOT_FINISHED ? "info" : "warn",
                   "manual_sync", "Manual GDO sync requested", data, err != ESP_OK && err != ESP_ERR_NOT_FINISHED);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(req, "409 Conflict");
    }
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":%s,\"result\":\"%s\"}",
             err == ESP_OK || err == ESP_ERR_NOT_FINISHED ? "true" : "false", esp_err_to_name(err));
    return send_json_response(req, json);
}

static esp_err_t gdo_position_post_handler(httpd_req_t *req) {
    std::string body;
    esp_err_t err = read_request_body(req, body, 512);
    if (err != ESP_OK) {
        return send_error_response(req, "400 Bad Request", "bad_request", esp_err_to_name(err));
    }
    if (!require_admin(req, &body)) {
        return ESP_OK;
    }

    int target_percent = -1;
    if (!json_get_int_value(body, "target_percent", &target_percent) || target_percent < 0 || target_percent > 100) {
        return send_error_response(req, "400 Bad Request", "invalid_target_percent");
    }

    gdo_status_t status = {};
    err = gdo_get_status(&status);
    if (err != ESP_OK) {
        return send_error_response(req, "409 Conflict", "gdo_status_unavailable", esp_err_to_name(err));
    }
    const bool door_moving = status.door == GDO_DOOR_STATE_OPENING || status.door == GDO_DOOR_STATE_CLOSING;
    const char *reason = nullptr;
    if (!status.synced) {
        reason = "not_synced";
    } else if (status.door_position < 0) {
        reason = "position_unknown";
    } else if (status.open_ms == 0 || status.close_ms == 0) {
        reason = "timing_unknown";
    } else if (door_moving) {
        reason = "door_moving";
    } else if (status.obstruction == GDO_OBSTRUCTION_STATE_OBSTRUCTED) {
        reason = "obstructed";
    }
    if (reason) {
        app_events_log("http", "warn", "partial_open_rejected", "Partial-open request rejected", "{}", false);
        return send_error_response(req, "409 Conflict", reason);
    }

    uint32_t target = (uint32_t)target_percent * 100U;
    err = gdo_door_move_to_target(target);
    char data[64];
    snprintf(data, sizeof(data), "{\"target_percent\":%d,\"result\":\"%s\"}", target_percent, esp_err_to_name(err));
    app_events_log("gdo", err == ESP_OK ? "info" : "warn", "partial_open",
                   err == ESP_OK ? "Partial-open command queued" : "Partial-open command failed",
                   data, err != ESP_OK);
    if (err != ESP_OK) {
        return send_error_response(req, "409 Conflict", "partial_open_failed", esp_err_to_name(err));
    }
    return send_json_response(req, "{\"ok\":true}");
}

static esp_err_t homekit_setup_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    int paired_count = homekit_paired_controller_count();
    char *payload = homekit_setup_payload();
    std::string json;
    json.reserve(192);
    json += "{\"paired_controller_count\":";
    if (paired_count >= 0) {
        json += std::to_string(paired_count);
    } else {
        json += "null";
    }
    json += ",\"setup_available\":";
    json += payload ? "true" : "false";
    json += ",\"setup_payload\":";
    if (payload) {
        json_string_value(json, payload);
        free(payload);
    } else {
        json += "null";
    }
    json += "}";
    return send_json_response(req, json);
}

static std::string build_setup_qr_svg(const char *payload) {
    static constexpr int QR_MAX_VERSION = 10;
    uint8_t qrcode[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)] = {};
    uint8_t temp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)] = {};
    bool ok = qrcodegen_encodeText(payload, temp, qrcode, qrcodegen_Ecc_LOW,
                                   qrcodegen_VERSION_MIN, QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
    if (!ok) {
        return "";
    }

    const int size = qrcodegen_getSize(qrcode);
    const int border = 4;
    const int view_size = size + border * 2;
    std::string svg;
    svg.reserve(4096);
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 ";
    svg += std::to_string(view_size);
    svg += " ";
    svg += std::to_string(view_size);
    svg += "\" shape-rendering=\"crispEdges\"><rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#111\" d=\"";
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (qrcodegen_getModule(qrcode, x, y)) {
                svg += "M";
                svg += std::to_string(x + border);
                svg += " ";
                svg += std::to_string(y + border);
                svg += "h1v1h-1z";
            }
        }
    }
    svg += "\"/></svg>";
    return svg;
}

static esp_err_t homekit_setup_qr_get_handler(httpd_req_t *req) {
    if (!web_access_allowed(req)) {
        return ESP_OK;
    }
    char *payload = homekit_setup_payload();
    if (!payload) {
        return send_error_response(req, "404 Not Found", "setup_unavailable");
    }
    std::string svg = build_setup_qr_svg(payload);
    free(payload);
    if (svg.empty()) {
        return send_error_response(req, "500 Internal Server Error", "qr_generation_failed");
    }
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, svg.c_str());
}

static esp_err_t app_register_http_handlers(httpd_handle_t server) {
    auto register_uri = [server](const char *path, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) -> esp_err_t {
        httpd_uri_t uri = {};
        uri.uri = path;
        uri.method = method;
        uri.handler = handler;
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %s", path, esp_err_to_name(err));
            return err;
        }
        return ESP_OK;
    };

    ESP_RETURN_ON_ERROR(register_uri("/api/access", HTTP_GET, access_get_handler), TAG, "access API");
    ESP_RETURN_ON_ERROR(register_uri("/api/status", HTTP_GET, status_get_handler), TAG, "status API");
    ESP_RETURN_ON_ERROR(register_uri("/ota", HTTP_GET, ota_page_get_handler), TAG, "OTA page");
    ESP_RETURN_ON_ERROR(register_uri("/api/ota", HTTP_GET, ota_get_handler), TAG, "OTA status API");
    ESP_RETURN_ON_ERROR(register_uri("/api/ota", HTTP_POST, ota_post_handler), TAG, "OTA upload API");
    ESP_RETURN_ON_ERROR(register_uri("/api/events", HTTP_GET, events_get_handler), TAG, "events API");
    ESP_RETURN_ON_ERROR(register_uri("/api/events/clear", HTTP_POST, events_clear_post_handler), TAG, "events clear API");
    ESP_RETURN_ON_ERROR(register_uri("/api/admin/setup", HTTP_POST, admin_setup_post_handler), TAG, "admin setup API");
    ESP_RETURN_ON_ERROR(register_uri("/api/admin/check", HTTP_POST, admin_check_post_handler), TAG, "admin check API");
    ESP_RETURN_ON_ERROR(register_uri("/api/settings", HTTP_GET, settings_get_handler), TAG, "settings API");
    ESP_RETURN_ON_ERROR(register_uri("/api/settings", HTTP_POST, settings_post_handler), TAG, "settings update API");
    ESP_RETURN_ON_ERROR(register_uri("/api/gdo/refresh", HTTP_POST, gdo_refresh_post_handler), TAG, "GDO refresh API");
    ESP_RETURN_ON_ERROR(register_uri("/api/gdo/sync", HTTP_POST, gdo_sync_post_handler), TAG, "GDO sync API");
    ESP_RETURN_ON_ERROR(register_uri("/api/gdo/position", HTTP_POST, gdo_position_post_handler), TAG, "GDO position API");
    ESP_RETURN_ON_ERROR(register_uri("/api/homekit/setup", HTTP_GET, homekit_setup_get_handler), TAG, "HomeKit setup API");
    ESP_RETURN_ON_ERROR(register_uri("/api/homekit/setup-qr.svg", HTTP_GET, homekit_setup_qr_get_handler), TAG, "HomeKit setup QR API");
    return ESP_OK;
}

static void app_wifi_start_config_server(int restart_mode) {
    nvs_wifi_connect_set_auth_handler(require_web_access);
    nvs_wifi_connect_set_write_auth_handler(require_admin_access);
    httpd_handle_t server = nvs_wifi_connect_start_http_server(restart_mode, app_register_http_handlers);
    if (server == nullptr) {
        ESP_LOGE(TAG, "Failed to start WiFi configuration HTTP server");
    }
}

static void app_wifi_event_logger(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    char data[96];
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        snprintf(data, sizeof(data), "{\"reason\":%d}", event ? static_cast<int>(event->reason) : 0);
        app_events_log("wifi", "warn", "sta_disconnected", "STA Wi-Fi disconnected", data, true);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        char ip[16] = {};
        if (event) {
            format_ip(&event->ip_info.ip, ip, sizeof(ip));
        }
        snprintf(data, sizeof(data), "{\"ip\":\"%s\"}", ip);
        app_events_log("wifi", "info", "sta_got_ip", "STA Wi-Fi got IP", data, false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        app_events_log("wifi", "info", "ap_client_connected", "SoftAP client connected", "{}", false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        app_events_log("wifi", "info", "ap_client_disconnected", "SoftAP client disconnected", "{}", false);
    }
}

static void app_wifi_register_event_logger(void)
{
    static bool registered;
    if (registered) {
        return;
    }
    esp_err_t err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, app_wifi_event_logger, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, app_wifi_event_logger, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, app_wifi_event_logger, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, app_wifi_event_logger, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to register Wi-Fi event logger: %s", esp_err_to_name(err));
        return;
    }
    registered = true;
}

void app_wifi_init(void) {
    if (nvs_wifi_connect() != ESP_OK) {
        app_wifi_register_event_logger();
        app_wifi_start_config_server(NVS_WIFI_CONNECT_MODE_RESTART_ESP32);
        return;
    }
    app_wifi_register_event_logger();
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_STA) {
        ESP_LOGI(TAG, "Connected in STA mode");
        app_wifi_start_config_server(NVS_WIFI_CONNECT_MODE_STAY_ACTIVE);
    } else if (mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Running in AP mode");
        app_wifi_start_config_server(NVS_WIFI_CONNECT_MODE_RESTART_ESP32);
    } else {
        ESP_LOGI(TAG, "WiFi not configured");
    }
}
