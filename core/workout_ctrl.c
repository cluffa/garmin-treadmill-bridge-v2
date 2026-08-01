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

#define FLAG_HAS_STEP            0x01

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

/* Decode the frame into the action the treadmill should take. Returns the
 * kind; *kmh is set for ACT_SPEED. */
static action_kind_t decode_action(const uint8_t *d, float *kmh)
{
    uint8_t timer_state = d[1];
    uint8_t flags       = d[2];
    uint8_t intensity   = d[3];
    uint8_t target_type = d[4];
    uint16_t low_mmps   = rd_u16(d + 5);
    uint16_t high_mmps  = rd_u16(d + 7);

    /* The belt moves only while the activity timer is running; paused, stopped
     * or pre-start all mean "ensure the belt is stopped". */
    if (timer_state != TIMER_STATE_ON) return ACT_STOP;

    /* An explicit speed target always wins, on any step including a rest one.
     * Only a speed target maps to belt speed — HR/power/cadence/open targets
     * carry none, and a step with no target at all (free run) sets neither the
     * flag nor a target type. */
    if ((flags & FLAG_HAS_STEP) && target_type == WORKOUT_STEP_TARGET_SPEED) {
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

/* ---- command application ------------------------------------------------ */

static void issue(action_kind_t kind, float kmh)
{
    if (kind == ACT_SPEED) machine_set_speed(kmh);
    else if (kind == ACT_STOP) machine_stop();
    s_ka_ticks = 0;
}

void workout_ctrl_on_frame(const uint8_t *data, uint16_t len)
{
    if (!data || len < WORKOUT_FRAME_LEN || data[0] != WORKOUT_FRAME_VERSION)
        return;

    float kmh = 0.0f;
    action_kind_t kind = decode_action(data, &kmh);

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
