#include "sdm_broadcast.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define TICK 0.25f          /* the footpod broadcasts at ~4 Hz */
#define NEAR(a, b) (fabsf((a) - (b)) < 0.01f)

static sdm_broadcast_t b;
static float spd, dst;

static void up(float v, float d)   { sdm_broadcast_step(&b, true,  v, d, TICK, &spd, &dst); }
static void down(void)             { sdm_broadcast_step(&b, false, 0, 0, TICK, &spd, &dst); }

int main(void)
{
    /* ---- a healthy link passes speed straight through, odometer tracks --- */
    sdm_broadcast_init(&b);
    up(2.855f, 100.0f);                 /* first tick only baselines */
    assert(NEAR(spd, 2.855f) && NEAR(dst, 0.0f));
    up(2.855f, 100.7f);
    assert(NEAR(spd, 2.855f) && NEAR(dst, 0.7f));

    /* ---- a deliberate stop is reported over a live link: no hold --------- *
     * This is the 18:53 case in 23928343294_ACTIVITY.fit — the user stopped
     * the belt at 2 mi and the treadmill said so while connected. Zero must
     * reach the air immediately, or the bridge would claim the runner was
     * still moving after they stepped off. */
    up(0.0f, 100.7f);
    assert(NEAR(spd, 0.0f));
    up(0.0f, 100.7f);
    assert(NEAR(spd, 0.0f) && NEAR(dst, 0.7f));   /* and no phantom distance */

    /* ---- link drop: speed HELD, odometer keeps advancing ----------------- *
     * The 23:34 case. The belt is still running; a comms event must not be
     * broadcast as a dead stop. */
    sdm_broadcast_init(&b);
    up(3.0f, 0.0f);
    up(3.0f, 0.75f);
    float d_before = dst;
    down();
    assert(NEAR(spd, 3.0f));                       /* held, not zeroed */
    assert(dst > d_before);                        /* still counting up */
    for (int i = 0; i < 4; i++) down();            /* 1.25 s down total */
    assert(NEAR(spd, 3.0f));
    assert(NEAR(dst, d_before + 3.0f * 5 * TICK));

    /* ---- past the grace window we admit we do not know ------------------- */
    while (b.down_s <= SDM_LINK_GRACE_S) down();
    float d_grace = dst;
    down();
    assert(NEAR(spd, 0.0f));                       /* honest zero */
    assert(NEAR(dst, d_grace));                    /* and stops accruing */

    /* ---- reconnect: source restarts at 0, odometer must NOT rewind ------- */
    float d_down = dst;
    up(3.0f, 0.0f);                                /* re-baseline, no jump */
    assert(NEAR(dst, d_down));
    up(3.0f, 0.75f);
    assert(NEAR(dst, d_down + 0.75f));             /* resumes from where it was */
    assert(NEAR(spd, 3.0f));

    /* ---- the FTMS flap: source alternates real <-> 0 --------------------- *
     * ftms_parse.c memsets its output and fills distance only when flag bit 2
     * is set, so a treadmill omitting the field on alternate notifications
     * flaps. Rejecting only negative deltas would bank the 0 -> real edge as
     * a kilometre-scale jump; both directions must be rejected. */
    sdm_broadcast_init(&b);
    up(3.0f, 4000.0f);
    up(3.0f, 4000.75f);
    float d_flap = dst;
    for (int i = 0; i < 20; i++) {
        up(3.0f, 0.0f);                            /* field absent   */
        up(3.0f, 4001.0f + (float)i);              /* field present  */
    }
    /* Every delta in that loop is implausible, so none is banked. */
    assert(NEAR(dst, d_flap));

    /* ---- monotonicity holds across an adversarial source ---------------- */
    sdm_broadcast_init(&b);
    float prev = 0.0f;
    const float srcs[] = {0, 10, 5, 10.5f, 0, 0.5f, 1e6f, 1.0f, 1.25f};
    for (unsigned i = 0; i < sizeof srcs / sizeof srcs[0]; i++) {
        up(2.0f, srcs[i]);
        assert(dst >= prev);                       /* never rewinds */
        prev = dst;
    }

    /* ---- a plausible delta at the clamp boundary is still accepted ------- */
    sdm_broadcast_init(&b);
    up(8.0f, 0.0f);
    float ok = SDM_MAX_BELT_MPS * TICK + SDM_DELTA_SLACK_M - 0.01f;
    up(8.0f, ok);
    assert(NEAR(dst, ok));

    printf("sdm_broadcast: OK\n");
    return 0;
}
