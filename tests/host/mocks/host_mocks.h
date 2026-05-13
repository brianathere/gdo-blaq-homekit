#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void host_nvs_reset(void);
void host_time_set_ms(uint32_t ms);
void host_time_advance_ms(uint32_t ms);
void host_random_seed(uint32_t seed);

#ifdef __cplusplus
}
#endif

