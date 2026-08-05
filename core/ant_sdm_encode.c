#include "ant_sdm_encode.h"
#include <math.h>

/* SDM speed field: 4-bit integer m/s (caps at 15 m/s = 54 km/h — far above
 * any treadmill) + 1/256 m/s fraction. */
static uint8_t speed_int(float mps) {
    if (mps < 0) return 0;
    uint32_t i = (uint32_t)mps;
    return (uint8_t)(i > 15 ? 15 : i);
}

static uint8_t speed_frac(float mps) {
    if (mps < 0) return 0;
    float f = mps - floorf(mps);
    long r = lroundf(f * 256.0f);
    return (uint8_t)(r > 255 ? 255 : r);
}

/* SDM time field: 1/256 s fraction byte + integer-second byte that rolls at
 * 256 s. Both halves matter. A receiver derives speed and stride rate by
 * differentiating the sensor's own distance and stride counters against this
 * clock, so leaving the fraction at 0 — as this did until 2026-08-05 — makes
 * the timestamp change only once a second while pages go out at ~4 Hz. Three
 * of every four pages then carry dt = 0 against their predecessor, which is
 * the degenerate case of exactly the division the receiver is trying to do. */
static uint8_t time_frac(float s) {
    if (s < 0) return 0;
    long r = lroundf((s - floorf(s)) * 256.0f);
    return (uint8_t)(r > 255 ? 255 : r);
}

void ant_sdm_encode_page1(const treadmill_state_t *s, uint8_t out[8]) {
    float dist = s->distance_m < 0 ? 0 : s->distance_m;
    uint32_t dist_int = (uint32_t)dist;
    long df = lroundf((dist - floorf(dist)) * 16.0f);
    uint8_t dist_frac = (uint8_t)(df > 15 ? 15 : df) & 0x0F;

    float t = s->elapsed_s < 0 ? 0 : s->elapsed_s;

    out[0] = 0x01;                              /* page number */
    out[1] = time_frac(t);                      /* time, 1/256 s */
    out[2] = (uint8_t)((uint32_t)t % 256);      /* time, integer s (rolls) */
    out[3] = (uint8_t)(dist_int % 256);         /* distance, integer m (rolls) */
    out[4] = (uint8_t)((dist_frac << 4) | speed_int(s->speed_mps));
    out[5] = speed_frac(s->speed_mps);          /* speed, 1/256 m/s */
    out[6] = 0x00;                              /* stride count (no sensor) */
    out[7] = 0x00;                              /* update latency */
}

/* Page 2 status byte (SDM profile, byte 7). Two bits each, LSB first:
 *   [1:0] use state   0 = inactive, 1 = ACTIVE
 *   [3:2] health      0 = OK
 *   [5:4] battery     0 = new
 *   [7:6] location    0 = laces
 * Use state must be 1. A zero says "this footpod is not in use", and a watch
 * that samples page 2 in that state records the speed as 0 — which is exactly
 * the isolated 0 km/h spikes test/pace_lag_report.py found in the 2026-08-01
 * trace, with the distance field advancing straight through them, proving the
 * belt never slowed. The SDK spells the enum out in
 * components/ant/ant_profiles/ant_sdm/pages/ant_sdm_page_2.h
 * (ANT_SDM_USE_STATE_ACTIVE = 0x01). */
#define SDM_STATUS_ACTIVE  0x01

/* Cadence is broadcast as the SDM "invalid" encoding — 0xFF integer byte,
 * 0xF fraction nibble — NOT 0x00. A treadmill has no stride sensor, and a
 * zero here is a *valid* reading of 0 strides/min: the watch believes the
 * footpod and stops falling back to its own wrist-derived cadence, so the
 * recorded activity shows 0 spm for the whole run. */
void ant_sdm_encode_page2(const treadmill_state_t *s, uint8_t out[8]) {
    out[0] = 0x02;                              /* page number */
    out[1] = 0xFF;                              /* reserved */
    out[2] = 0xFF;                              /* reserved */
    out[3] = 0xFF;                              /* cadence, integer: invalid */
    out[4] = (uint8_t)(0xF0 | speed_int(s->speed_mps)); /* cad frac | speed int */
    out[5] = speed_frac(s->speed_mps);          /* speed, 1/256 m/s */
    out[6] = 0x00;                              /* reserved */
    out[7] = SDM_STATUS_ACTIVE;                 /* use state active, health OK */
}
