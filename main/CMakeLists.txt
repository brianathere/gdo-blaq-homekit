idf_component_register(SRCS "gdo-blaq-homekit.cpp" "homekit.cpp" "wifi.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES gdolib esp_hap_core esp_hap_apple_profiles esp_event esp_netif esp_wifi nvs_flash nvs_wifi_connect
                )
