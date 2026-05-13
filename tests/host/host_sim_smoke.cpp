#include <assert.h>
#include <stdio.h>

#include <string>

#include "app_events.h"
#include "app_settings.h"
#include "gdo_host.h"
#include "host_mocks.h"

int main(void)
{
    host_nvs_reset();
    host_gdo_reset();
    host_time_set_ms(1234);
    host_random_seed(1);

    assert(app_events_init() == ESP_OK);
    assert(app_settings_init() == ESP_OK);

    app_events_log("boot", "warn", "reset", "Boot reset reason: software",
                   "{\"reason\":\"software\"}", true);
    app_events_log("gdo", "info", "gdo_synced", "GDO synchronized",
                   "{\"synced\":true,\"protocol\":\"Security+ 2.0\"}", true);

    app_settings_t settings = {};
    app_settings_get(&settings);
    settings.open_ms = 9000;
    settings.close_ms = 10000;
    settings.min_command_interval_ms = 200;
    assert(app_settings_save_and_apply(&settings, nullptr, nullptr) == ESP_OK);

    std::string events = app_events_build_json(10, nullptr, nullptr);
    std::string app_settings = app_settings_build_json();
    assert(events.find("\"critical_events\":2") != std::string::npos);
    assert(app_settings.find("\"open_ms\":9000") != std::string::npos);

    printf("%s\n%s\n", events.c_str(), app_settings.c_str());
    return 0;
}

