#pragma once
#include <stdint.h>

typedef struct {
    float speed_mps;
    float distance_m;
    float incline_pct;
    uint32_t elapsed_s;
} treadmill_state_t;
