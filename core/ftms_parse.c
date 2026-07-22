#include "ftms_parse.h"
#include <string.h>

static uint16_t rd_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int16_t  rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
static uint32_t rd_u24(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16); }

bool ftms_parse_treadmill_data(const uint8_t *buf, size_t len,
                               treadmill_state_t *out) {
    if (len < 2) return false;
    uint16_t flags = rd_u16(buf);
    size_t i = 2;
    memset(out, 0, sizeof *out);

    // Field order & presence per FTMS Treadmill Data (0x2ACD).
    // bit0 == 0 means Instantaneous Speed is present.
    if (!(flags & (1 << 0))) {
        if (i + 2 > len) return false;
        out->speed_mps = rd_u16(buf + i) * 0.01f * 1000.0f / 3600.0f;
        i += 2;
    }
    if (flags & (1 << 1)) i += 2;                 // average speed
    if (flags & (1 << 2)) {                       // total distance (uint24, m)
        if (i + 3 > len) return false;
        out->distance_m = (float)rd_u24(buf + i);
        i += 3;
    }
    if (flags & (1 << 3)) {                        // inclination + ramp angle
        if (i + 4 > len) return false;
        out->incline_pct = rd_s16(buf + i) * 0.1f;
        i += 4;
    }
    if (flags & (1 << 4)) i += 4;                  // elevation gain (pos+neg)
    if (flags & (1 << 5)) i += 1;                  // instantaneous pace
    if (flags & (1 << 6)) i += 1;                  // average pace
    if (flags & (1 << 7)) i += 5;                  // expended energy (2+2+1)
    if (flags & (1 << 8)) i += 1;                  // heart rate
    if (flags & (1 << 9)) i += 1;                  // metabolic equivalent
    if (flags & (1 << 10)) {                       // elapsed time (s)
        if (i + 2 > len) return false;
        out->elapsed_s = rd_u16(buf + i);
        i += 2;
    }
    return true;
}
