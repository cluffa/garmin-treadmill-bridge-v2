#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ant_sdm_encode.h"

int main(void) {
    /* 3.0 m/s, 100.0 m, t=0 -> exact integer case */
    treadmill_state_t a = { .speed_mps = 3.0f, .distance_m = 100.0f,
                            .incline_pct = 0.0f, .elapsed_s = 0 };
    uint8_t p1[8];
    ant_sdm_encode_page1(&a, p1);
    uint8_t exp_a[8] = {0x01,0x00,0x00,0x64,0x03,0x00,0x00,0x00};
    assert(memcmp(p1, exp_a, 8) == 0);

    /* 3.5 m/s, 100.5 m -> fractional nibbles exercised */
    treadmill_state_t b = { .speed_mps = 3.5f, .distance_m = 100.5f,
                            .incline_pct = 0.0f, .elapsed_s = 0 };
    ant_sdm_encode_page1(&b, p1);
    uint8_t exp_b[8] = {0x01,0x00,0x00,0x64,0x83,0x80,0x00,0x00};
    assert(memcmp(p1, exp_b, 8) == 0);

    /* time + distance rollover: 300 s -> 300%256=44; 1000.0 m -> 1000%256=232 */
    treadmill_state_t c = { .speed_mps = 3.0f, .distance_m = 1000.0f,
                            .incline_pct = 0.0f, .elapsed_s = 300.0f };
    ant_sdm_encode_page1(&c, p1);
    assert(p1[2] == 44 && p1[3] == 232);

    /* Fractional time must reach byte 1 (1/256 s). The footpod broadcasts at
     * ~4 Hz, so whole-second-only timestamps repeat across three of every four
     * pages and a receiver differentiating distance or strides against this
     * clock hits dt = 0 on those. 12.25 s -> int 12, frac 0.25*256 = 64. */
    treadmill_state_t t = { .speed_mps = 3.0f, .distance_m = 0.0f,
                            .incline_pct = 0.0f, .elapsed_s = 12.25f };
    ant_sdm_encode_page1(&t, p1);
    assert(p1[1] == 64 && p1[2] == 12);

    /* Successive pages one quarter-second apart must differ in the time
     * field — the property the fractional byte exists to guarantee. */
    uint8_t prev[8];
    ant_sdm_encode_page1(&t, prev);
    for (int k = 1; k <= 4; k++) {
        treadmill_state_t n = t;
        n.elapsed_s = 12.25f + 0.25f * (float)k;
        ant_sdm_encode_page1(&n, p1);
        assert(p1[1] != prev[1] || p1[2] != prev[2]);
        memcpy(prev, p1, 8);
    }

    /* The fraction must never round up into the next whole second: the
     * integer byte is a floor, so 255/256 is the ceiling for byte 1. */
    treadmill_state_t e = { .speed_mps = 0.0f, .distance_m = 0.0f,
                            .incline_pct = 0.0f, .elapsed_s = 41.9999f };
    ant_sdm_encode_page1(&e, p1);
    assert(p1[2] == 41 && p1[1] == 255);

    uint8_t p2[8];
    ant_sdm_encode_page2(&b, p2);
    assert(p2[0] == 0x02 && (p2[4] & 0x0F) == 0x03 && p2[5] == 0x80);
    assert(p2[1] == 0xFF && p2[2] == 0xFF);
    /* status byte: use state must read ACTIVE (bits[1:0] == 1). A 0 here means
     * "footpod not in use" and the watch records speed 0 whenever it samples a
     * page 2 — the 0 km/h spikes seen in test/23806153959_ACTIVITY.fit.
     * health/battery/location all stay 0 (OK / new / laces). */
    assert(p2[7] == 0x01);
    assert((p2[7] & 0x03) == 0x01);   /* use state  = active */
    assert((p2[7] & 0x0C) == 0x00);   /* health     = OK     */
    /* cadence must be the SDM "invalid" encoding (0xFF integer, 0xF fraction),
     * not 0x00 — a zero cadence is a *valid* 0 strides/min and stops the watch
     * from falling back to its own wrist cadence. */
    assert(p2[3] == 0xFF && (p2[4] & 0xF0) == 0xF0);

    /* stopped belt encodes zero speed */
    treadmill_state_t z = { .speed_mps = 0.0f, .distance_m = 0.0f,
                            .incline_pct = 0.0f, .elapsed_s = 10 };
    ant_sdm_encode_page1(&z, p1);
    assert((p1[4] & 0x0F) == 0 && p1[5] == 0);

    printf("ant_sdm_encode: OK\n");
    return 0;
}
