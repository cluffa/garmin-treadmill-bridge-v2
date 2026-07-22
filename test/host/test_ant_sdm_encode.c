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
                            .incline_pct = 0.0f, .elapsed_s = 300 };
    ant_sdm_encode_page1(&c, p1);
    assert(p1[2] == 44 && p1[3] == 232);

    uint8_t p2[8];
    ant_sdm_encode_page2(&b, p2);
    assert(p2[0] == 0x02 && (p2[4] & 0x0F) == 0x03 && p2[5] == 0x80);
    assert(p2[1] == 0xFF && p2[2] == 0xFF && p2[7] == 0x00);

    /* stopped belt encodes zero speed */
    treadmill_state_t z = { .speed_mps = 0.0f, .distance_m = 0.0f,
                            .incline_pct = 0.0f, .elapsed_s = 10 };
    ant_sdm_encode_page1(&z, p1);
    assert((p1[4] & 0x0F) == 0 && p1[5] == 0);

    printf("ant_sdm_encode: OK\n");
    return 0;
}
