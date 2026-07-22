#include "ifit_parse.h"

bool ifit_parse_data(const uint8_t *b, size_t len,
                     float *speed_mps, float *incline_pct) {
    if (len != 20) return false;
    /* bytes 0-3: frame type; byte 5: model discriminator 0x2e for T6.5S data frames.
     * Other frame types (ack, status) share the same 0-3 header but have different
     * byte[5] and carry zeros in the speed/incline fields — reject them here. */
    if (b[0] != 0x00 || b[1] != 0x12 || b[2] != 0x01 || b[3] != 0x04 || b[5] != 0x2e)
        return false;

    uint16_t spd = (uint16_t)b[10] | ((uint16_t)b[11] << 8);          /* 0.01 km/h */
    int16_t  inc = (int16_t)((uint16_t)b[12] | ((uint16_t)b[13] << 8)); /* 0.01 %  */

    *speed_mps   = (spd / 100.0f) * (1000.0f / 3600.0f);
    *incline_pct = inc / 100.0f;
    return true;
}
