#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <esp_err.h>

typedef struct {
    uint32_t seq;
    uint32_t time_ms;
    char category[16];
    char severity[8];
    char code[32];
    char message[96];
    char data_json[96];
    bool persisted;
} app_event_record_t;

typedef struct {
    uint32_t total_events;
    uint32_t dropped_events;
    uint32_t critical_events;
    bool has_last_critical;
    app_event_record_t last_critical;
} app_events_summary_t;

esp_err_t app_events_init(void);
void app_events_log(const char *category, const char *severity, const char *code,
                    const char *message, const char *data_json, bool persist);
void app_events_logf(const char *category, const char *severity, const char *code,
                     bool persist, const char *data_json, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
esp_err_t app_events_clear(void);
void app_events_get_summary(app_events_summary_t *summary);
std::string app_events_build_json(size_t limit, const char *category, const char *severity);
