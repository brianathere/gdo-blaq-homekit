#include "app_health.h"

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <hap.h>

#include "app_events.h"

static const char *TAG = "app_health";

static constexpr uint32_t HEALTH_CHECK_INTERVAL_MS = 30 * 1000;
static constexpr uint32_t HOMEKIT_START_REBOOT_MS = 5 * 60 * 1000;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 60 * 1000;
static constexpr uint32_t WIFI_REBOOT_AFTER_MS = 30 * 60 * 1000;
static constexpr uint32_t GDO_RESYNC_INTERVAL_MS = 5 * 60 * 1000;
static constexpr uint32_t GDO_PROBE_INTERVAL_MS = 5 * 60 * 1000;
static constexpr uint32_t GDO_REBOOT_AFTER_MS = 30 * 60 * 1000;
static constexpr uint32_t WATCHDOG_FEED_INTERVAL_MS = 1000;
static constexpr uint32_t REBOOT_GRACE_MS = 5000;

static TaskHandle_t s_health_task;
static portMUX_TYPE s_health_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_homekit_started;
static uint32_t s_started_ms;
static uint32_t s_last_gdo_event_ms;

static uint32_t millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool elapsed(uint32_t now, uint32_t since, uint32_t interval)
{
    return (uint32_t)(now - since) >= interval;
}

const char *app_health_reset_reason_to_string(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
}

static bool homekit_started(void)
{
    bool started;
    portENTER_CRITICAL(&s_health_lock);
    started = s_homekit_started;
    portEXIT_CRITICAL(&s_health_lock);
    return started;
}

esp_err_t app_health_get_snapshot(app_health_snapshot_t *snapshot)
{
    if (!snapshot) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_health_lock);
    snapshot->homekit_started = s_homekit_started;
    snapshot->started_ms = s_started_ms;
    snapshot->last_gdo_event_ms = s_last_gdo_event_ms;
    portEXIT_CRITICAL(&s_health_lock);
    return ESP_OK;
}

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) {
        return false;
    }

    esp_netif_ip_info_t ip = {};
    if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) {
        return false;
    }

    return ip.ip.addr != 0;
}

static void reboot_for_health(const char *reason)
{
    ESP_LOGE(TAG, "Health check failed: %s. Rebooting.", reason);
    char data[96];
    snprintf(data, sizeof(data), "{\"reason\":\"%s\"}", reason ? reason : "unknown");
    app_events_log("watchdog", "error", "health_reboot", "Health supervisor requested reboot", data, true);
    if (homekit_started() && hap_reboot_accessory() == HAP_SUCCESS) {
        vTaskDelay(pdMS_TO_TICKS(REBOOT_GRACE_MS));
    }
    esp_restart();
}

static void check_wifi(uint32_t now, uint32_t *wifi_down_since, uint32_t *last_reconnect_ms)
{
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read WiFi mode: %s", esp_err_to_name(err));
        return;
    }

    const bool sta_mode = mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA;
    if (!sta_mode || sta_has_ip()) {
        *wifi_down_since = 0;
        return;
    }

    if (*wifi_down_since == 0) {
        *wifi_down_since = now;
    }

    if (*last_reconnect_ms == 0 || elapsed(now, *last_reconnect_ms, WIFI_RECONNECT_INTERVAL_MS)) {
        ESP_LOGW(TAG, "STA has no IP; requesting reconnect");
        app_events_log("wifi", "warn", "wifi_reconnect", "STA has no IP; reconnect requested", "{}", true);
        esp_wifi_connect();
        *last_reconnect_ms = now;
    }

    if (elapsed(now, *wifi_down_since, WIFI_REBOOT_AFTER_MS)) {
        reboot_for_health("STA WiFi has been disconnected too long");
    }
}

static void check_gdo(uint32_t now, uint32_t *unsynced_since, uint32_t *last_resync_ms, uint32_t *last_probe_ms)
{
    gdo_status_t status = {};
    esp_err_t err = gdo_get_status(&status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to read GDO status: %s", esp_err_to_name(err));
        return;
    }

    if (!status.synced) {
        if (*unsynced_since == 0) {
            *unsynced_since = now;
        }

        if (*last_resync_ms == 0 || elapsed(now, *last_resync_ms, GDO_RESYNC_INTERVAL_MS)) {
            err = gdo_sync();
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "GDO resync request failed: %s", esp_err_to_name(err));
                char data[48];
                snprintf(data, sizeof(data), "{\"result\":\"%s\"}", esp_err_to_name(err));
                app_events_log("gdo", "warn", "health_resync_failed", "GDO health resync request failed", data, true);
            }
            *last_resync_ms = now;
        }

        if (elapsed(now, *unsynced_since, GDO_REBOOT_AFTER_MS)) {
            reboot_for_health("GDO has been unsynced too long");
        }
        return;
    }

    *unsynced_since = 0;
    *last_resync_ms = 0;

    uint32_t last_rx_ms = gdo_get_last_rx_ms();
    if (last_rx_ms == 0) {
        return;
    }

    if ((*last_probe_ms == 0 || elapsed(now, *last_probe_ms, GDO_PROBE_INTERVAL_MS)) &&
        elapsed(now, last_rx_ms, GDO_PROBE_INTERVAL_MS)) {
        err = gdo_request_status();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "GDO health status request failed: %s", esp_err_to_name(err));
            char data[48];
            snprintf(data, sizeof(data), "{\"result\":\"%s\"}", esp_err_to_name(err));
            app_events_log("gdo", "warn", "health_status_failed", "GDO health status request failed", data, true);
        }
        *last_probe_ms = now;
    }

    if (elapsed(now, last_rx_ms, GDO_REBOOT_AFTER_MS)) {
        reboot_for_health("GDO has not produced valid RX traffic too long");
    }
}

static void app_health_task(void *arg)
{
    (void)arg;

    uint32_t wifi_down_since = 0;
    uint32_t last_reconnect_ms = 0;
    uint32_t unsynced_since = 0;
    uint32_t last_resync_ms = 0;
    uint32_t last_probe_ms = 0;
    uint32_t last_check_ms = 0;

    esp_err_t err = esp_task_wdt_add(NULL);
    const bool watchdog_subscribed = err == ESP_OK || err == ESP_ERR_INVALID_ARG;
    if (!watchdog_subscribed && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Unable to subscribe health task to TWDT: %s", esp_err_to_name(err));
    }

    for (;;) {
        const uint32_t now = millis();
        if (watchdog_subscribed) {
            (void)esp_task_wdt_reset();
        }

        if (last_check_ms == 0 || elapsed(now, last_check_ms, HEALTH_CHECK_INTERVAL_MS)) {
            last_check_ms = now;

            if (!homekit_started() && elapsed(now, s_started_ms, HOMEKIT_START_REBOOT_MS)) {
                reboot_for_health("HomeKit did not start");
            }

            check_wifi(now, &wifi_down_since, &last_reconnect_ms);
            check_gdo(now, &unsynced_since, &last_resync_ms, &last_probe_ms);

            if (watchdog_subscribed) {
                (void)esp_task_wdt_reset();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_FEED_INTERVAL_MS));
    }
}

esp_err_t app_health_start(void)
{
    if (s_health_task) {
        return ESP_ERR_INVALID_STATE;
    }

    s_started_ms = millis();
    s_last_gdo_event_ms = s_started_ms;

    ESP_LOGI(TAG, "Reset reason: %s", app_health_reset_reason_to_string(esp_reset_reason()));

    if (xTaskCreate(app_health_task, "app_health", 4096, NULL, 2, &s_health_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void app_health_mark_homekit_started(void)
{
    portENTER_CRITICAL(&s_health_lock);
    s_homekit_started = true;
    portEXIT_CRITICAL(&s_health_lock);
    ESP_LOGI(TAG, "HomeKit marked healthy");
}

void app_health_mark_gdo_event(const gdo_status_t *status, gdo_cb_event_t event)
{
    (void)status;
    (void)event;
    portENTER_CRITICAL(&s_health_lock);
    s_last_gdo_event_ms = millis();
    portEXIT_CRITICAL(&s_health_lock);
}
