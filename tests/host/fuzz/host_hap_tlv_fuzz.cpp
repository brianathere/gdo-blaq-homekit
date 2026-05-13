#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>

extern "C" {
#include "esp_hap_pair_common.h"
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!data || size == 0) {
        return 0;
    }

    const uint8_t type = data[0];
    uint8_t value[512] = {};
    const size_t value_len = std::min(size - 1, sizeof(value));
    memcpy(value, data + 1, value_len);

    uint8_t tlv_buf[768] = {};
    hap_tlv_data_t tlv = {};
    hap_tlv_data_init(&tlv, tlv_buf, sizeof(tlv_buf));
    (void)add_tlv(&tlv, type, static_cast<int>(value_len), value);
    if (size > 4) {
        (void)add_tlv(&tlv, data[1], static_cast<int>(std::min<size_t>(size - 2, 260)), const_cast<uint8_t *>(data + 2));
    }

    uint8_t out[768] = {};
    (void)get_tlv_length(tlv_buf, tlv.curlen, type);
    (void)get_value_from_tlv(tlv_buf, tlv.curlen, type, out, sizeof(out));
    if (tlv.curlen > 0) {
        (void)get_tlv_length(tlv_buf, tlv.curlen - 1, type);
        (void)get_value_from_tlv(tlv_buf, tlv.curlen - 1, type, out, sizeof(out));
    }

    int outlen = 0;
    hap_prepare_error_tlv(STATE_M2, kTLVError_Authentication, tlv_buf, sizeof(tlv_buf), &outlen);
    return 0;
}
