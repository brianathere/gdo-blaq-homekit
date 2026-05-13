#include <assert.h>
#include <stdint.h>

#include "secplus.h"

uint32_t nondet_uint32_t(void);
uint64_t nondet_uint64_t(void);

void secplus_smoke_harness(void)
{
    const uint32_t rolling_v1 = UINT32_C(0x12345678);
    const uint32_t fixed_v1 = UINT32_C(123456789);
    uint8_t symbols1[20];
    uint8_t symbols2[20];
    uint32_t decoded_rolling_v1 = 0;
    uint32_t decoded_fixed_v1 = 0;

    assert(encode_v1(rolling_v1, fixed_v1, symbols1, symbols2) == 0);
    assert(decode_v1(symbols1, symbols2, &decoded_rolling_v1, &decoded_fixed_v1) == 0);
    assert(decoded_rolling_v1 == (rolling_v1 & UINT32_C(0xfffffffe)));
    assert(decoded_fixed_v1 == fixed_v1);

    const uint32_t rolling_v2 = UINT32_C(0x01234567);
    const uint64_t fixed_v2 = UINT64_C(0x0123456789);
    const uint32_t data_v2 = UINT32_C(0x00abcdef);
    uint8_t packet1[8];
    uint8_t packet2[8];
    uint8_t wireline[19];
    uint32_t decoded_rolling_v2 = 0;
    uint64_t decoded_fixed_v2 = 0;
    uint32_t decoded_data_v2 = 0;

    assert(encode_v2(rolling_v2, fixed_v2, data_v2, 1, packet1, packet2) == 0);
    assert(decode_v2(1, packet1, packet2, &decoded_rolling_v2, &decoded_fixed_v2, &decoded_data_v2) == 0);
    assert(decoded_rolling_v2 == rolling_v2);
    assert(decoded_fixed_v2 == fixed_v2);
    assert((decoded_data_v2 & UINT32_C(0xffff0fff)) == (data_v2 & UINT32_C(0xffff0fff)));

    decoded_rolling_v2 = 0;
    decoded_fixed_v2 = 0;
    decoded_data_v2 = 0;
    assert(encode_wireline(rolling_v2, fixed_v2, data_v2, wireline) == 0);
    assert(decode_wireline(wireline, &decoded_rolling_v2, &decoded_fixed_v2, &decoded_data_v2) == 0);
    assert(decoded_rolling_v2 == rolling_v2);
    assert(decoded_fixed_v2 == fixed_v2);
    assert((decoded_data_v2 & UINT32_C(0xffff0fff)) == (data_v2 & UINT32_C(0xffff0fff)));
}

void secplus_v1_roundtrip_harness(void)
{
    uint32_t rolling = nondet_uint32_t();
    uint32_t fixed = nondet_uint32_t();
    uint8_t symbols1[20];
    uint8_t symbols2[20];
    uint32_t decoded_rolling = 0;
    uint32_t decoded_fixed = 0;

    __CPROVER_assume(rolling < UINT32_C(0x10000));
    __CPROVER_assume(fixed < UINT32_C(59049));

    assert(encode_v1(rolling, fixed, symbols1, symbols2) == 0);
    assert(decode_v1(symbols1, symbols2, &decoded_rolling, &decoded_fixed) == 0);
    assert(decoded_rolling == (rolling & UINT32_C(0xfffffffe)));
    assert(decoded_fixed == fixed);
}

void secplus_v2_roundtrip_harness(void)
{
    uint32_t rolling = nondet_uint32_t();
    uint64_t fixed = nondet_uint64_t();
    uint32_t data = nondet_uint32_t();
    uint8_t packet1[8];
    uint8_t packet2[8];
    uint32_t decoded_rolling = 0;
    uint64_t decoded_fixed = 0;
    uint32_t decoded_data = 0;

    __CPROVER_assume(rolling < UINT32_C(0x10000000));
    __CPROVER_assume(fixed < UINT64_C(0x10000000000));

    assert(encode_v2(rolling, fixed, data, 1, packet1, packet2) == 0);
    assert(decode_v2(1, packet1, packet2, &decoded_rolling, &decoded_fixed, &decoded_data) == 0);
    assert(decoded_rolling == rolling);
    assert(decoded_fixed == fixed);
    assert((decoded_data & UINT32_C(0xffff0fff)) == (data & UINT32_C(0xffff0fff)));
}

void secplus_wireline_roundtrip_harness(void)
{
    uint32_t rolling = nondet_uint32_t();
    uint64_t fixed = nondet_uint64_t();
    uint32_t data = nondet_uint32_t();
    uint8_t packet[19];
    uint32_t decoded_rolling = 0;
    uint64_t decoded_fixed = 0;
    uint32_t decoded_data = 0;

    __CPROVER_assume(rolling < UINT32_C(0x10000000));
    __CPROVER_assume(fixed < UINT64_C(0x10000000000));

    assert(encode_wireline(rolling, fixed, data, packet) == 0);
    assert(decode_wireline(packet, &decoded_rolling, &decoded_fixed, &decoded_data) == 0);
    assert(decoded_rolling == rolling);
    assert(decoded_fixed == fixed);
    assert((decoded_data & UINT32_C(0xffff0fff)) == (data & UINT32_C(0xffff0fff)));
}
