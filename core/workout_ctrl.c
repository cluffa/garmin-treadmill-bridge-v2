#include "workout_ctrl.h"
#include "machine.h"

/* ---- CIQ enum values mirrored from Toybox.Activity (the watch sends raw
 * enum ints; these must match the SDK constants). --------------------------- */
#define TIMER_STATE_OFF          0
#define TIMER_STATE_STOPPED      1
#define TIMER_STATE_PAUSED       2
#define TIMER_STATE_ON           3
#define WORKOUT_STEP_TARGET_SPEED 0
#define WORKOUT_INTENSITY_REST    1
#define WORKOUT_STEP_DURATION_TIME 0

#define FLAG_HAS_STEP            0x01
#define FLAG_HAS_NEXT            0x20   /* v2: bytes 15-18 carry the next step */
#define FLAG_ADV_UP_ONLY         0x40   /* v2: pre-roll speed increases only */

/* Sentinels in the v2 remaining_s field. "far" means the watch knows the
 * remaining time but is deliberately not spending a 1 Hz write stream on it
 * because the boundary is outside the window the bridge could act on. Both are
 * "do not pre-roll", for different reasons. */
#define REMAIN_UNKNOWN          0xFFFFu
#define REMAIN_FAR              0xFFFEu

/* Upper bound on the watch's advance_s. The watch clamps too; this is here so a
 * corrupt byte cannot pre-empt an entire step. */
#define ADVANCE_MAX_S             30

/* Speed commanded on a rest step that carries no usable speed target of its
 * own. Rest steps come off the watch as intensity=1(rest) with an OPEN target,
 * so without this the belt would hold the work-interval speed straight through
 * the rest. Commanded unconditionally, not as a floor: a work interval slower
 * than this would be sped up, which is accepted (see the design doc). */
#define REST_SPEED_KMH          4.0f

/* Keepalive: re-assert the current speed every ~30 s (tick is called at ~1 Hz)
 * so a lost write self-heals without the old 5 s spam. */
#define KEEPALIVE_TICKS         30

/* Two speeds within this many km/h are "the same" target — avoids re-commanding
 * (and, on iFit, re-triggering the console countdown) for rounding noise. */
#define SPEED_EPS_KMH           0.05f

typedef enum { ACT_NONE, ACT_SPEED, ACT_STOP } action_kind_t;

static action_kind_t s_last_kind = ACT_NONE;
static float         s_last_kmh;        /* valid when s_last_kind == ACT_SPEED */
static int           s_ka_ticks;        /* ticks since the last command issued */

/* ---- frame decode ------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Resolve one step slot (current or next) to the speed it wants.
 *
 * Returns ACT_SPEED with *kmh set, or ACT_NONE for "this step says nothing
 * about belt speed". Never returns ACT_STOP — the timer state decides that,
 * not a step. Shared by both slots so the next step is resolved by exactly the
 * rules the current one is; a caller that has a has-step flag folds it in by
 * passing 0xFF for target_type.
 *
 * For the next slot the watch has already reduced the range to its midpoint, so
 * it passes that value as both low and high. */
static action_kind_t resolve_speed(uint8_t intensity, uint8_t target_type,
                                   uint16_t low_mmps, uint16_t high_mmps,
                                   float *kmh)
{
    /* An explicit speed target always wins, on any step including a rest one.
     * Only a speed target maps to belt speed — HR/power/cadence/open targets
     * carry none, and a step with no target at all (free run) sets neither the
     * flag nor a target type. */
    if (target_type == WORKOUT_STEP_TARGET_SPEED) {
        /* Resolve the range to a midpoint; tolerate a one-sided target. */
        uint16_t mmps;
        if (low_mmps && high_mmps)  mmps = (uint16_t)((low_mmps + high_mmps) / 2);
        else                        mmps = low_mmps ? low_mmps : high_mmps;
        if (mmps) {
            *kmh = mmps * 0.0036f;    /* (mm/s / 1000) * 3.6 */
            return ACT_SPEED;
        }
        /* A 0 mm/s target is no target at all — fall through. */
    }

    /* No usable speed target. A rest step means "ease off", so walk it out
     * rather than holding the work-interval speed. Note the watch has been
     * observed reporting warmup and cooldown as rest too, which is fine: a
     * 4 km/h warmup/cooldown walk is the desired behaviour anyway. */
    if (intensity == WORKOUT_INTENSITY_REST) {
        *kmh = REST_SPEED_KMH;
        return ACT_SPEED;
    }

    /* Free run (intensity is the watch's 0xFF sentinel), or an active step
     * whose target is not a speed — hold whatever the belt is already doing
     * rather than stopping it, which would force a slow belt restart. */
    return ACT_NONE;
}

/* Decode the frame into the action the treadmill should take. Returns the
 * kind; *kmh is set for ACT_SPEED.
 *
 * A pure function of the frame: no countdown, no per-step state here. The
 * watch supplies the remaining time, so the same frame always produces the
 * same command — which is what lets test/pace_lag_report.py mirror this in
 * Python and test/mock/workout_probe.c expose it over a scalars-only ABI. */
static action_kind_t decode_action(const uint8_t *d, uint16_t len, float *kmh)
{
    uint8_t timer_state = d[1];
    uint8_t flags       = d[2];

    /* The belt moves only while the activity timer is running; paused, stopped
     * or pre-start all mean "ensure the belt is stopped". This outranks any
     * pre-roll: pausing during the lead-in stops the belt like any other. */
    if (timer_state != TIMER_STATE_ON) return ACT_STOP;

    float cur_kmh = 0.0f;
    action_kind_t cur = resolve_speed(d[3],
                                      (flags & FLAG_HAS_STEP) ? d[4] : 0xFF,
                                      rd_u16(d + 5), rd_u16(d + 7), &cur_kmh);

    /* ---- speed advance (v2 only) ----------------------------------------
     * The belt takes seconds to ramp. If the watch has told us what the next
     * step wants and how long this one has left, command the next speed
     * advance_s early so the belt is at pace when the watch's step starts.
     * Everything needed is in the frame; see workout_ctrl.h for the layout. */
    if (d[0] >= 2 && len >= WORKOUT_FRAME_LEN) {
        uint8_t  advance_s = d[19];
        uint16_t remaining = rd_u16(d + 12);
        uint8_t  dur_type  = d[9];
        uint16_t dur_value = rd_u16(d + 10);

        if (advance_s > ADVANCE_MAX_S) advance_s = ADVANCE_MAX_S;

        if (advance_s > 0
            && (flags & FLAG_HAS_NEXT)
            && remaining != REMAIN_UNKNOWN
            && remaining != REMAIN_FAR
            && remaining <= advance_s
            /* Short-step guard: a step shorter than twice the advance would be
             * pre-empted almost as soon as it started, so it runs at its own
             * speed and the next one is commanded at the boundary, as before.
             * Only TIME durations state their length; a distance step relies on
             * remaining_s alone. */
            && !(dur_type == WORKOUT_STEP_DURATION_TIME
                 && dur_value < (uint16_t)(2u * advance_s))) {

            float nxt_kmh = 0.0f;
            uint16_t nxt_mmps = rd_u16(d + 17);
            action_kind_t nxt = resolve_speed(d[15], d[16],
                                              nxt_mmps, nxt_mmps, &nxt_kmh);

            /* A next step with no usable speed (HR/open active step, or a free
             * run) pre-rolls nothing and the current command holds — the same
             * thing that happens at the boundary today.
             *
             * FLAG_ADV_UP_ONLY is the user's opt-out from easing *down* early:
             * with it set, only a faster next step is pre-rolled, so the full
             * work interval runs at work pace and the rest starts late. */
            if (nxt == ACT_SPEED
                && !((flags & FLAG_ADV_UP_ONLY)
                     && cur == ACT_SPEED && nxt_kmh < cur_kmh)) {
                *kmh = nxt_kmh;
                return ACT_SPEED;
            }
        }
    }

    if (cur == ACT_SPEED) *kmh = cur_kmh;
    return cur;
}

/* ---- command application ------------------------------------------------ */

static void issue(action_kind_t kind, float kmh)
{
    if (kind == ACT_SPEED) machine_set_speed(kmh);
    else if (kind == ACT_STOP) machine_stop();
    s_ka_ticks = 0;
}

void workout_ctrl_on_frame(const uint8_t *data, uint16_t len)
{
    if (!data || len < WORKOUT_FRAME_LEN_V1) return;

    /* Both wire versions are accepted so an older data-field build keeps
     * driving new firmware. Each version brings its own minimum length; a v2
     * frame that is short is as unusable as a v1 frame that is short. */
    uint16_t need;
    if      (data[0] == 1)                     need = WORKOUT_FRAME_LEN_V1;
    else if (data[0] == WORKOUT_FRAME_VERSION) need = WORKOUT_FRAME_LEN;
    else                                       return;
    if (len < need) return;

    float kmh = 0.0f;
    action_kind_t kind = decode_action(data, len, &kmh);

    /* ACT_NONE = "don't touch the belt": keep the last command latched so the
     * keepalive holds the belt where it is. */
    if (kind == ACT_NONE) return;

    bool changed = (kind != s_last_kind) ||
                   (kind == ACT_SPEED &&
                    (kmh - s_last_kmh > SPEED_EPS_KMH ||
                     s_last_kmh - kmh > SPEED_EPS_KMH));
    if (!changed) return;

    s_last_kind = kind;
    s_last_kmh  = kmh;
    issue(kind, kmh);   /* apply on change immediately — no waiting for tick */
}

void workout_ctrl_tick(void)
{
    if (s_ka_ticks < KEEPALIVE_TICKS * 2) s_ka_ticks++;
    /* Re-assert an active speed only; a stopped belt needs no keepalive. */
    if (s_last_kind == ACT_SPEED && s_ka_ticks >= KEEPALIVE_TICKS)
        issue(ACT_SPEED, s_last_kmh);
}

void workout_ctrl_reset(void)
{
    s_last_kind = ACT_NONE;
    s_last_kmh  = 0.0f;
    s_ka_ticks  = 0;
}

/* Record a manual command from ctrl_dispatch so the keepalive re-asserts the
 * *manual* value, not the last workout-step target.  The caller has already
 * issued the command to the machine; we just update internal state. */
void workout_ctrl_note_manual(int kind, float kmh)
{
    if (kind == WORKOUT_CTRL_ACT_STOP) {
        s_last_kind = ACT_STOP;
        s_ka_ticks  = 0;
    } else if (kind == WORKOUT_CTRL_ACT_SPEED) {
        s_last_kind = ACT_SPEED;
        s_last_kmh  = kmh;
        s_ka_ticks  = 0;
    }
    /* WORKOUT_CTRL_ACT_NONE (0) is a no-op — used if the caller just wants
     * to reset the keepalive without changing the latched command. */
}
