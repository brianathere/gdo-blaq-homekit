#ifndef APP_HEALTH_H
#define APP_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_system.h>
#include <gdo.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool homekit_started;
    uint32_t started_ms;
    uint32_t last_gdo_event_ms;
} app_health_snapshot_t;

esp_err_t app_health_start(void);
esp_err_t app_health_get_snapshot(app_health_snapshot_t *snapshot);
void app_health_mark_homekit_started(void);
void app_health_mark_gdo_event(const gdo_status_t *status, gdo_cb_event_t event);
const char *app_health_reset_reason_to_string(esp_reset_reason_t reason);

#ifdef __cplusplus
}
#endif

#endif // APP_HEALTH_H
