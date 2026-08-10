#include "sdm_broadcast.h"

void sdm_broadcast_init(sdm_broadcast_t *b)
{
    b->dist_m   = 0.0f;
    b->src_prev = 0.0f;
    b->have_src = false;
    b->hold_mps = 0.0f;
    b->down_s   = 0.0f;
}

void sdm_broadcast_step(sdm_broadcast_t *b, bool link_up,
                        float src_speed_mps, float src_dist_m, float dt_s,
                        float *out_speed_mps, float *out_dist_m)
{
    if (dt_s < 0.0f) dt_s = 0.0f;

    if (link_up) {
        b->down_s = 0.0f;

        /* A reported zero over a healthy link is a real stop — pass it
         * through untouched. The grace hold below is only ever about a link
         * that went away, never about a belt that slowed down. */
        float v = src_speed_mps < 0.0f ? 0.0f : src_speed_mps;
        b->hold_mps = v;

        /* Accumulate the source odometer by deltas, rejecting implausible
         * ones in BOTH directions (see the header: negative on reset,
         * positive on an FTMS field flap). A rejected delta re-baselines, so
         * the next tick measures from where the source actually is. */
        if (b->have_src) {
            float delta = src_dist_m - b->src_prev;
            float limit = SDM_MAX_BELT_MPS * dt_s + SDM_DELTA_SLACK_M;
            if (delta > 0.0f && delta <= limit) {
                b->dist_m += delta;
            }
        }
        b->src_prev = src_dist_m;
        b->have_src = true;
    } else {
        b->down_s += dt_s;

        if (b->down_s <= SDM_LINK_GRACE_S) {
            /* The belt is still running; keep speed and odometer consistent
             * with each other so a receiver differentiating distance against
             * the clock gets the same answer the speed field reports. */
            b->dist_m += b->hold_mps * dt_s;
        } else {
            b->hold_mps = 0.0f;
        }

        /* The source is zeroed while down, and restarts from 0 on reconnect.
         * Drop the baseline so the first tick back re-establishes it instead
         * of differencing against a stale pre-drop value. */
        b->have_src = false;
    }

    *out_speed_mps = b->hold_mps;
    *out_dist_m    = b->dist_m;
}
