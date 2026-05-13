#include "gdo_host.h"

#include <cstring>

namespace {

gdo_status_t g_status = {};
uint32_t g_last_rx_ms;
uint32_t g_min_command_interval_ms;
esp_err_t g_next_apply_result = ESP_OK;

void set_defaults(void)
{
    std::memset(&g_status, 0, sizeof(g_status));
    g_status.protocol = GDO_PROTOCOL_SEC_PLUS_V2;
    g_status.door = GDO_DOOR_STATE_CLOSED;
    g_status.light = GDO_LIGHT_STATE_OFF;
    g_status.lock = GDO_LOCK_STATE_UNLOCKED;
    g_status.motion = GDO_MOTION_STATE_CLEAR;
    g_status.obstruction = GDO_OBSTRUCTION_STATE_CLEAR;
    g_status.motor = GDO_MOTOR_STATE_OFF;
    g_status.button = GDO_BUTTON_STATE_RELEASED;
    g_status.battery = GDO_BATT_STATE_UNKNOWN;
    g_status.learn = GDO_LEARN_STATE_INACTIVE;
    g_status.synced = true;
    g_status.open_ms = 12000;
    g_status.close_ms = 13000;
    g_min_command_interval_ms = 0;
    g_last_rx_ms = 1000;
    g_next_apply_result = ESP_OK;
}

} // namespace

extern "C" void host_gdo_reset(void)
{
    set_defaults();
}

extern "C" void host_gdo_set_status(const gdo_status_t *status)
{
    if (status) {
        g_status = *status;
    }
}

extern "C" void host_gdo_get_applied_settings(gdo_status_t *status, uint32_t *last_rx_ms)
{
    if (status) {
        *status = g_status;
    }
    if (last_rx_ms) {
        *last_rx_ms = g_last_rx_ms;
    }
}

extern "C" uint32_t host_gdo_get_min_command_interval_ms(void)
{
    return g_min_command_interval_ms;
}

extern "C" esp_err_t gdo_get_status(gdo_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    *status = g_status;
    return ESP_OK;
}

extern "C" uint32_t gdo_get_last_rx_ms(void)
{
    return g_last_rx_ms;
}

extern "C" esp_err_t gdo_set_protocol(gdo_protocol_type_t protocol)
{
    if (g_next_apply_result != ESP_OK) {
        return g_next_apply_result;
    }
    if (protocol != GDO_PROTOCOL_MAX) {
        g_status.protocol = protocol;
    }
    return ESP_OK;
}

extern "C" esp_err_t gdo_set_open_duration(uint16_t ms)
{
    if (g_next_apply_result != ESP_OK) {
        return g_next_apply_result;
    }
    g_status.open_ms = ms;
    return ESP_OK;
}

extern "C" esp_err_t gdo_set_close_duration(uint16_t ms)
{
    if (g_next_apply_result != ESP_OK) {
        return g_next_apply_result;
    }
    g_status.close_ms = ms;
    return ESP_OK;
}

extern "C" esp_err_t gdo_set_min_command_interval(uint32_t ms)
{
    if (g_next_apply_result != ESP_OK) {
        return g_next_apply_result;
    }
    g_min_command_interval_ms = ms;
    return ESP_OK;
}

extern "C" void gdo_set_toggle_only(bool toggle_only)
{
    g_status.toggle_only = toggle_only;
}

extern "C" const char *gdo_protocol_type_to_string(gdo_protocol_type_t type)
{
    switch (type) {
    case GDO_PROTOCOL_SEC_PLUS_V1:
        return "Security+ 1.0";
    case GDO_PROTOCOL_SEC_PLUS_V2:
        return "Security+ 2.0";
    case GDO_PROTOCOL_SEC_PLUS_V1_WITH_SMART_PANEL:
        return "Security+ 1.0 with smart panel";
    default:
        return "Unknown";
    }
}
