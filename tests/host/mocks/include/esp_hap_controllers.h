#pragma once

#include <stdint.h>

#define HAP_CTRL_ID_LEN 64

typedef struct {
    char id[HAP_CTRL_ID_LEN];
    uint8_t ltpk[32];
    uint8_t admin;
} hap_ctrl_data_t;

