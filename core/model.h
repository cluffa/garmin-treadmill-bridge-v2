#pragma once
#include <stdint.h>

typedef struct {
    float speed_mps;
    float distance_m;
    float incline_pct;
    /* Seconds, fractional. Float rather than whole seconds because the SDM
     * page-1 time field has a 1/256 s fractional byte and the footpod
     * broadcasts at ~4 Hz: at whole-second resolution three of every four
     * pages repeat the same timestamp, so a receiver differentiating against
     * this clock divides by zero three times a second. See
     * core/ant_sdm_encode.c ant_sdm_encode_page1(). */
    float elapsed_s;
} treadmill_state_t;
