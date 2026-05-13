#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA2_256 0

esp_err_t esp_sha(int sha_type, const unsigned char *input, size_t ilen, unsigned char output[32]);

#ifdef __cplusplus
}
#endif

