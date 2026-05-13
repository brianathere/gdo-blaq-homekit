#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_fill_random(void *buf, size_t len);
uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif

