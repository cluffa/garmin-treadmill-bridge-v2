#pragma once
/*
 * sdm_broadcast.h — what the ANT footpod broadcasts for speed and distance,
 * decoupled from the treadmill BLE link.
 *
 * WHY THIS EXISTS
 * ---------------
 * A footpod measures the foot. It does not know or care whether some other
 * radio is currently connected. Until 2026-08-10 the bridge broadcast the
 * treadmill state struct directly, and the firmware's BLE-central glue zeroes
 * that struct on disconnect ("belt stops when the link drops" — a false
 * premise, annotated at the memset), so a 3-second link dropout
 * made the footpod announce a dead stop to the watch while the runner was
 * still going 6.4 mph.
 *
 * Observed in 23928343294_ACTIVITY.fit (2026-08-10, timer 23:34): speed
 * stepped 2.855 -> 0.000 for three seconds and the recorded distance froze,
 * then resumed with no catch-up. Both fingerprints say "the data source went
 * away", not "the belt decelerated" — a belt slowing from 2.855 m/s still
 * covers ground, and it cannot reach zero within one sample.
 *
 * This is the same defect class the 2026-08-05 clock fix addressed one field
 * over: that one stopped page 1's *time* coming from the treadmill; this one
 * stops its *speed and distance* being reset by a link event.
 *
 * THE LINK-DOWN GRACE HOLD
 * ------------------------
 * On link loss the last known speed is held for SDM_LINK_GRACE_S, and the
 * odometer keeps advancing at that speed, because the belt really is still
 * running — a dropped BLE link is a comms event, not a belt event. Past the
 * grace window we genuinely do not know, and 0 becomes the honest answer.
 *
 * This does NOT paper over a real stop. When the user stops the belt the link
 * stays UP and the treadmill *reports* speed 0, which passes straight through
 * with no hold. The same .FIT shows exactly that at 18:53: a deliberate stop
 * at the 2 mi mark, reported as zero over a healthy link.
 *
 * THE MONOTONIC ODOMETER
 * ----------------------
 * A footpod odometer only ever counts up. The source resets to 0 on reconnect
 * (the central's s_distance_m accumulator) and again on the disconnect memset,
 * so the broadcast distance is accumulated from *deltas* of the source, and
 * an implausible delta re-baselines instead of accumulating.
 *
 * The clamp is not merely defensive. core/ftms_parse.c memsets its output and
 * fills distance only when Treadmill Data flag bit 2 is present, so an FTMS
 * treadmill that omits that field on alternate notifications makes the source
 * flap real -> 0 -> real. Skipping only *negative* deltas would then treat
 * the 0 -> real edge as a huge forward jump and add kilometres per flap.
 * Rejecting both directions is what makes this safe against reset paths
 * nobody has enumerated yet.
 */

#include <stdbool.h>

/* Hold the last known speed this long after the link drops before admitting
 * we do not know. Longer than any reconnect observed; short enough that a
 * genuine unnoticed stop overstates distance by only a few metres. */
#define SDM_LINK_GRACE_S   5.0f

/* Fastest belt we will believe, m/s (~18 mph — beyond any treadmill's top
 * speed). Anything implying more than this between two ticks is a source
 * reset or a flap, not motion. The +1 m of slack absorbs notification jitter,
 * since the source advances on the treadmill's schedule, not on our ticks. */
#define SDM_MAX_BELT_MPS   8.0f
#define SDM_DELTA_SLACK_M  1.0f

typedef struct {
    float dist_m;    /* broadcast odometer, m — monotonic non-decreasing */
    float src_prev;  /* last accepted source distance, m */
    bool  have_src;  /* src_prev is a valid baseline */
    float hold_mps;  /* last speed seen with the link up */
    float down_s;    /* seconds accumulated since the link went down */
} sdm_broadcast_t;

void sdm_broadcast_init(sdm_broadcast_t *b);

/* Advance by dt_s and produce what the footpod should put on the air.
 *
 *   link_up        — treadmill BLE link is established
 *   src_speed_mps  — belt speed as last parsed (meaningless when !link_up)
 *   src_dist_m     — belt odometer as last parsed (resets; may flap)
 *
 * out_speed_mps / out_dist_m may not be NULL. */
void sdm_broadcast_step(sdm_broadcast_t *b, bool link_up,
                        float src_speed_mps, float src_dist_m, float dt_s,
                        float *out_speed_mps, float *out_dist_m);
