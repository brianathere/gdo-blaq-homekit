#include <stddef.h>
#include <stdint.h>

#include "secplus.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint32_t rolling32 = 0;
    uint64_t fixed64 = 0;
    uint32_t data32 = 0;

    if (size >= 40) {
        (void)decode_v1(data, data + 20, &rolling32, &data32);
    }

    if (size >= 16) {
        (void)decode_v2(data[0] & 1, data, data + 8, &rolling32, &fixed64, &data32);
    }

    if (size >= 19) {
        (void)decode_wireline(data, &rolling32, &fixed64, &data32);
    }

    return 0;
}

