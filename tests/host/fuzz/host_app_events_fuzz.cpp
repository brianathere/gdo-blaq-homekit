#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "app_events.h"
#include "host_mocks.h"

static void bounded_string(char *out, size_t out_size, const uint8_t *data, size_t size)
{
    if (!out || out_size == 0) {
        return;
    }
    size_t n = std::min(size, out_size - 1);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = data[i];
        out[i] = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : 'x';
    }
    out[n] = 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size < 4) {
        return 0;
    }

    host_time_set_ms(data[0]);
    (void)app_events_init();
    (void)app_events_clear();

    const char *categories[] = {"app", "boot", "gdo", "homekit", "wifi", "watchdog", "http", "settings"};
    const char *severities[] = {"info", "warn", "error"};
    const char *codes[] = {"reset", "sta_disconnected", "gdo_sync_failed", "rolling_code_recovery", "door_position"};

    char message[96];
    bounded_string(message, sizeof(message), data + 4, size - 4);

    for (size_t i = 0; i < 8 && (4 + i) < size; ++i) {
        char json[96];
        snprintf(json, sizeof(json), "{\"reason\":%u,\"attempt\":%u}", data[1], data[2]);
        app_events_log(categories[data[i] % 8],
                       severities[data[(i + 1) % size] % 3],
                       codes[data[(i + 2) % size] % 5],
                       message,
                       json,
                       (data[(i + 3) % size] & 1) != 0);
        host_time_advance_ms(1);
    }

    (void)app_events_build_json(data[3], categories[data[1] % 8], severities[data[2] % 3]);
    app_events_summary_t summary = {};
    app_events_get_summary(&summary);
    return 0;
}
