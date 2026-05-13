#pragma once

#include <stdint.h>

#include <esp_err.h>
#include <gdo.h>

#ifdef __cplusplus
extern "C" {
#endif

void host_gdo_reset(void);
void host_gdo_set_status(const gdo_status_t *status);
void host_gdo_get_applied_settings(gdo_status_t *status, uint32_t *last_rx_ms);
uint32_t host_gdo_get_min_command_interval_ms(void);

#ifdef __cplusplus
}
#endif
