#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "ftms_parse.h"

static int approx(float a, float b) { return fabsf(a - b) < 0.001f; }

int main(void) {
    // flags: bit2 (total distance) | bit3 (inclination) | bit10 (elapsed) = 0x040C
    // bit0 == 0 => instantaneous speed present
    uint8_t buf[] = {
        0x0C, 0x04,             // flags
        0x20, 0x03,             // speed = 800 (0.01 km/h) = 8.00 km/h
        0xD2, 0x04, 0x00,       // total distance = 1234 m (uint24)
        0x0F, 0x00,             // inclination = 15 (0.1%) = 1.5%
        0x00, 0x00,             // ramp angle = 0
        0x41, 0x00,             // elapsed time = 65 s
    };
    treadmill_state_t s = {0};
    assert(ftms_parse_treadmill_data(buf, sizeof buf, &s));
    assert(approx(s.speed_mps, 8.0f * 1000.0f / 3600.0f)); // 2.2222 m/s
    assert(approx(s.distance_m, 1234.0f));
    assert(approx(s.incline_pct, 1.5f));
    assert(s.elapsed_s == 65);
    printf("ftms_parse: OK\n");
    return 0;
}
