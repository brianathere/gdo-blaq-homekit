idf_component_register(SRCS "gdo-blaq-homekit.cpp" "homekit.cpp" "wifi.cpp" "app_health.cpp" "app_events.cpp" "app_settings.cpp" "../esp-homekit-sdk/examples/common/qrcode/src/qrcodegen.c"
                    INCLUDE_DIRS "." "../esp-homekit-sdk/examples/common/qrcode/src"
                    REQUIRES gdolib app_update esp_app_format esp_partition esp_hap_core esp_hap_apple_profiles esp_event esp_http_server esp_netif esp_timer esp_wifi heap json_parser mbedtls nvs_flash nvs_wifi_connect
                )
