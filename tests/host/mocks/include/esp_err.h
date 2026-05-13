#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL 0x101
#define ESP_ERR_NO_MEM 0x102
#define ESP_ERR_INVALID_ARG 0x103
#define ESP_ERR_INVALID_STATE 0x104
#define ESP_ERR_INVALID_SIZE 0x105
#define ESP_ERR_NOT_FOUND 0x106
#define ESP_ERR_NOT_FINISHED 0x107
#define ESP_ERR_TIMEOUT 0x108
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
#define ESP_ERR_WIFI_NOT_INIT 0x3001

const char *esp_err_to_name(esp_err_t err);

#ifdef __cplusplus
}
#endif

