#include "app_events.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

static const char *TAG = "app_events";

static constexpr size_t RAM_EVENT_COUNT = 128;
static constexpr size_t CRITICAL_EVENT_COUNT = 32;
static constexpr uint32_t EVENTS_BLOB_MAGIC = 0x47444f45; // GDOE
static constexpr uint32_t EVENTS_BLOB_VERSION = 1;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t next;
    app_event_record_t records[CRITICAL_EVENT_COUNT];
} persisted_events_blob_t;

static SemaphoreHandle_t s_lock;
static app_event_record_t s_events[RAM_EVENT_COUNT];
static size_t s_event_count;
static size_t s_event_next;
static app_event_record_t s_critical[CRITICAL_EVENT_COUNT];
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

static void append_record_locked(const app_event_record_t *record)
{
    if (s_event_count == RAM_EVENT_COUNT) {
        ++s_dropped_events;
    } else {
        ++s_event_count;
    }
    s_events[s_event_next] = *record;
    s_event_next = (s_event_next + 1) % RAM_EVENT_COUNT;

    if (record->seq >= s_next_seq) {
        s_next_seq = record->seq + 1;
    }
}

static void save_critical_events(void)
{
    persisted_events_blob_t blob = {};
    blob.magic = EVENTS_BLOB_MAGIC;
    blob.version = EVENTS_BLOB_VERSION;

    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        blob.count = (uint32_t)s_critical_count;
        blob.next = (uint32_t)s_critical_next;
        memcpy(blob.records, s_critical, sizeof(blob.records));
        xSemaphoreGive(s_lock);
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("gdo_diag", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to open diagnostics NVS: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_blob(nvs, "crit_events", &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to persist diagnostics events: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

static void add_critical_locked(app_event_record_t *record)
{
    record->persisted = true;
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_initialized) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    persisted_events_blob_t blob = {};
    size_t blob_size = sizeof(blob);
    nvs_handle_t nvs = 0;
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
                s_critical[idx].persisted = true;
                append_record_locked(&s_critical[idx]);
            }
        }
    }

    s_initialized = true;
    xSemaphoreGive(s_lock);
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
    if (should_persist) {
        add_critical_locked(&record);
    }
    append_record_locked(&record);
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_events, 0, sizeof(s_events));
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
        summary->last_critical = s_critical[idx];
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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t emitted = 0;
    for (size_t i = 0; i < s_event_count && emitted < limit; ++i) {
        size_t idx = (s_event_next + RAM_EVENT_COUNT - 1 - i) % RAM_EVENT_COUNT;
        const app_event_record_t &event = s_events[idx];
        if (category && category[0] && strcmp(event.category, category) != 0) {
            continue;
        }
        if (severity && severity[0] && strcmp(event.severity, severity) != 0) {
            continue;
        }
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
