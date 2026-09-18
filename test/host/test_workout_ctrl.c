#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "workout_ctrl.h"
#include "machine.h"

/* ---- machine_* stubs: record what workout_ctrl commanded ---- */
static float g_last_speed;
static int   g_speed_calls;
static int   g_stop_calls;

void   machine_start_scan(void) {}
int    machine_get_devices(ftms_device_t *o, int m) { (void)o; (void)m; return 0; }
void   machine_connect(const ftms_device_t *d) { (void)d; }
bool   machine_connected(void) { return true; }
const ftms_device_t *machine_connected_device(void) { return NULL; }
bool   machine_set_incline(float p) { (void)p; return true; }
bool   machine_set_speed(float kmh) { g_last_speed = kmh; g_speed_calls++; return true; }
bool   machine_stop(void) { g_stop_calls++; return true; }

/* ---- frame builder ---- */
static void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

#define TIMER_ON 3
#define TIMER_PAUSED 2
#define TIMER_STOPPED 1
#define TIMER_OFF 0
#define TGT_SPEED 0
#define TGT_HR 1
#define TGT_OPEN 2
#define TGT_NONE   0xFF   /* watch sentinel: no next step */
#define INT_ACTIVE 0
#define INT_REST   1
#define INT_NONE   0xFF   /* watch sentinel: no step resolved */
#define DUR_TIME   0
#define DUR_DIST   1
#define DUR_NONE   0xFF

#define FLAG_HAS_NEXT    0x20
#define FLAG_ADV_UP_ONLY 0x40

#define REMAIN_FAR     0xFFFE
#define REMAIN_UNKNOWN 0xFFFF

/* timer_state, has_step, target_type, low/high in mm/s, intensity.
 *
 * Builds a *v1* frame (version 1, WORKOUT_FRAME_LEN_V1 bytes): every test that
 * predates the speed-advance work is a v1 test and must keep passing verbatim
 * against the v2 firmware. The buffer is the full v2 length and zeroed whole,
 * so a v1 frame sent with sizeof(f) has no stale next-slot bytes behind it. */
static void frame_i(uint8_t *f, uint8_t timer, bool has_step, uint8_t target,
                    uint16_t low_mmps, uint16_t high_mmps, uint8_t intensity)
{
    memset(f, 0, WORKOUT_FRAME_LEN);
    f[0] = 1;
    f[1] = timer;
    f[2] = has_step ? 0x01 : 0x00;
    f[3] = intensity;
    f[4] = target;
    put_u16(f + 5, low_mmps);
    put_u16(f + 7, high_mmps);
}

/* A v2 frame: the current slot exactly as frame_i builds it, plus the duration,
 * the remaining time, the next slot and the advance the pre-roll rule reads. */
static void frame2(uint8_t *f, uint8_t timer, bool has_step, uint8_t target,
                   uint16_t low_mmps, uint16_t high_mmps, uint8_t intensity,
                   uint8_t dur_type, uint16_t dur_value, uint16_t remaining,
                   bool has_next, uint8_t next_intensity, uint8_t next_target,
                   uint16_t next_mmps, uint8_t advance_s, bool up_only)
{
    memset(f, 0, WORKOUT_FRAME_LEN);
    f[0] = WORKOUT_FRAME_VERSION;
    f[1] = timer;
    f[2] = (uint8_t)((has_step ? 0x01 : 0x00) |
                     (has_next ? FLAG_HAS_NEXT : 0x00) |
                     (up_only  ? FLAG_ADV_UP_ONLY : 0x00));
    f[3] = intensity;
    f[4] = target;
    put_u16(f + 5, low_mmps);
    put_u16(f + 7, high_mmps);
    f[9] = dur_type;
    put_u16(f + 10, dur_value);
    put_u16(f + 12, remaining);
    f[14] = 0;
    f[15] = next_intensity;
    f[16] = next_target;
    put_u16(f + 17, next_mmps);
    f[19] = advance_s;
}

/* The workout used by every pre-roll test below: a 60 s work step at ~8.5 km/h
 * (2361 mm/s) whose next step runs at ~12.0 km/h (3333 mm/s), 5 s advance. */
#define WORK_MMPS  2361
#define NEXT_MMPS  3333
#define ADV        5

static void work_frame(uint8_t *f, uint16_t remaining, bool has_next,
                       uint8_t next_intensity, uint8_t next_target,
                       uint16_t next_mmps, uint8_t advance_s)
{
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 60, remaining, has_next, next_intensity, next_target,
           next_mmps, advance_s, false);
}

/* Same, defaulting to an ACTIVE (work) step — what every pre-rest-speed test
 * meant when it left byte [3] zeroed. */
static void frame(uint8_t *f, uint8_t timer, bool has_step, uint8_t target,
                  uint16_t low_mmps, uint16_t high_mmps)
{
    frame_i(f, timer, has_step, target, low_mmps, high_mmps, INT_ACTIVE);
}

static void reset_counts(void) { g_speed_calls = g_stop_calls = 0; g_last_speed = -1; }

int main(void)
{
    uint8_t f[WORKOUT_FRAME_LEN];
    workout_ctrl_reset();

    /* Running, speed target 10.0–10.0 km/h (2778 mm/s) → command 10.0 immediately. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    assert(g_last_speed > 9.9f && g_last_speed < 10.1f);

    /* Identical frame again → no re-command (dedup on change). */
    reset_counts();
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Range 8.0–9.0 km/h → midpoint 8.5 (2222–2500 mm/s → 2361 → ~8.5). */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2222, 2500);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    assert(g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* Keepalive: no new frame; re-asserts the last speed after ~30 ticks. */
    reset_counts();
    for (int i = 0; i < 29; i++) workout_ctrl_tick();
    assert(g_speed_calls == 0);          /* not yet */
    workout_ctrl_tick();                 /* 30th */
    assert(g_speed_calls == 1);
    assert(g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* Pause → stop the belt immediately. */
    reset_counts();
    frame(f, TIMER_PAUSED, true, TGT_SPEED, 2222, 2500);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 1 && g_speed_calls == 0);

    /* A stopped belt gets no keepalive churn. */
    reset_counts();
    for (int i = 0; i < 40; i++) workout_ctrl_tick();
    assert(g_stop_calls == 0 && g_speed_calls == 0);

    /* Resume → re-command the speed. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2222, 2500);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);

    /* ACTIVE step with an OPEN target (no speed) → hold; belt keeps moving
     * (no stop, no new speed), and keepalive still re-asserts the held speed.
     * Only intensity=REST gets the 4 km/h fallback — see the rest block below. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 0 && g_speed_calls == 0);
    for (int i = 0; i < 30; i++) workout_ctrl_tick();
    assert(g_speed_calls == 1);          /* still holding the last work speed */
    assert(g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* Rest step WITH an explicit speed target → the target wins over the
     * 4 km/h rest fallback. */
    reset_counts();
    frame_i(f, TIMER_ON, true, TGT_SPEED, 1389, 1389, INT_REST);   /* ~5.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_stop_calls == 0);
    assert(g_last_speed > 4.9f && g_last_speed < 5.1f);

    /* Free run (no step) while running → leave the belt alone. The watch sends
     * 0xFF sentinels for intensity/targetType when no step resolved, so this
     * must not be mistaken for a rest step. */
    reset_counts();
    frame_i(f, TIMER_ON, false, 0xFF, 0, 0, INT_NONE);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Non-speed target (HR) on an ACTIVE step → hold, no command. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_HR, 150, 160);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* ---- rest steps drop the belt to a 4 km/h walk ---- */

    /* Work → rest: the rest step arrives as intensity=1(rest) tgt=2(OPEN) with
     * flags bit0 clear (observed on hardware 2026-07-31), and must command
     * 4 km/h rather than holding the work speed. */
    reset_counts();
    workout_ctrl_reset();
    frame(f, TIMER_ON, true, TGT_SPEED, 2361, 2361);       /* work ~8.5 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    reset_counts();
    frame_i(f, TIMER_ON, false, TGT_OPEN, 0, 0, INT_REST);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_stop_calls == 0);
    assert(g_last_speed > 3.9f && g_last_speed < 4.1f);

    /* Keepalive during rest re-asserts 4 km/h, not the work speed. */
    reset_counts();
    for (int i = 0; i < 30; i++) workout_ctrl_tick();
    assert(g_speed_calls == 1);
    assert(g_last_speed > 3.9f && g_last_speed < 4.1f);

    /* Repeated rest frames dedup — one command per rest step, not per frame. */
    reset_counts();
    workout_ctrl_on_frame(f, sizeof f);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Rest → work: back up to the work speed. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2361, 2361);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* A rest step whose speed target resolves to 0 falls back to 4 km/h. */
    reset_counts();
    frame_i(f, TIMER_ON, true, TGT_SPEED, 0, 0, INT_REST);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_stop_calls == 0);
    assert(g_last_speed > 3.9f && g_last_speed < 4.1f);

    /* Pausing during a rest step still stops the belt. */
    reset_counts();
    frame_i(f, TIMER_PAUSED, false, TGT_OPEN, 0, 0, INT_REST);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 1 && g_speed_calls == 0);

    /* Malformed frames are ignored: bad version, short length. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);
    f[0] = 0x99;
    workout_ctrl_on_frame(f, sizeof f);
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);
    workout_ctrl_on_frame(f, WORKOUT_FRAME_LEN_V1 - 1);
    assert(g_speed_calls == 0 && g_stop_calls == 0);
    /* A v2-versioned frame that is only v1-long is short for *its* version. */
    frame2(f, TIMER_ON, true, TGT_SPEED, 2778, 2778, INT_ACTIVE,
           DUR_NONE, 0, REMAIN_UNKNOWN, false, INT_NONE, TGT_NONE, 0, 0, false);
    workout_ctrl_on_frame(f, WORKOUT_FRAME_LEN_V1);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Stopped timer → stop belt. */
    reset_counts();
    workout_ctrl_reset();
    frame(f, TIMER_STOPPED, true, TGT_SPEED, 2778, 2778);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 1);

    /* ---- FIX 2: manual STOP during active workout must latch the stop so
     * the keepalive does NOT re-assert the old speed. ---- */
    reset_counts();
    workout_ctrl_reset();
    /* Start an active speed via workout frame. */
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 9.9f);
    /* User manually presses STOP on the watch app. */
    reset_counts();
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_STOP, 0);
    /* Tick well past KEEPALIVE_TICKS — belt MUST stay stopped. */
    for (int i = 0; i < 40; i++) workout_ctrl_tick();
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* ---- FIX 2: manual SPEED during active workout latches the new speed
     * so the keepalive re-asserts the MANUAL value, not the workout target. ---- */
    reset_counts();
    workout_ctrl_reset();
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);   /* workout wants 10 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    /* User manually sets 7.0 km/h via the watch app.
     * note_manual() only latches the state (the caller, cmd_speed, already
     * issued machine_set_speed). The keepalive must re-assert the manual value. */
    reset_counts();
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_SPEED, 7.0f);
    assert(g_speed_calls == 0);   /* note_manual does NOT issue — cmd_speed did */
    /* Keepalive must re-assert the manual 7.0, not the workout's 10.0. */
    for (int i = 0; i < 30; i++) workout_ctrl_tick();
    assert(g_speed_calls == 1);
    assert(g_last_speed > 6.9f && g_last_speed < 7.1f);

    /* =====================================================================
     * Wire format v2: speed advance (pre-roll). The bridge commands the NEXT
     * step's speed advance_s seconds before the boundary so the belt has
     * finished ramping when the watch's step starts.
     * ===================================================================== */

    /* A v2 frame that carries no next step decodes exactly like v1: the
     * current step's speed, nothing early. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, 2, false, INT_NONE, TGT_NONE, 0, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* advance_s = 0 turns the feature off even with a next step in hand. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, 2, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* remaining_s unknown (0xFFFF) or "far" (0xFFFE): the watch is telling us
     * it cannot or will not say, so there is nothing to pre-roll against. */
    for (int k = 0; k < 2; k++) {
        reset_counts();
        workout_ctrl_reset();
        work_frame(f, k ? REMAIN_UNKNOWN : REMAIN_FAR,
                   true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
        workout_ctrl_on_frame(f, sizeof f);
        assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);
    }

    /* remaining == advance is inside the window; advance + 1 is not. Both are
     * checked from the same latched work speed so only the boundary moves. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, ADV + 1, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    reset_counts();
    work_frame(f, ADV, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    /* The rest of the lead-in re-sends the same pre-rolled target each second
     * and is deduplicated; so is the boundary frame, where the pre-rolled
     * speed finally shows up in the *current* slot. That dedup matters on
     * iFit, where a repeated set re-triggers the console countdown. */
    reset_counts();
    for (uint16_t rem = ADV - 1; rem > 0; rem--) {
        work_frame(f, rem, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
        workout_ctrl_on_frame(f, sizeof f);
    }
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    reset_counts();
    frame2(f, TIMER_ON, true, TGT_SPEED, NEXT_MMPS, NEXT_MMPS, INT_ACTIVE,
           DUR_TIME, 60, REMAIN_FAR, false, INT_NONE, TGT_NONE, 0, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Next step is a rest with an OPEN target: it resolves through the same
     * function the current slot uses, so the belt eases to the 4 km/h walk
     * 5 s early instead of at the boundary. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, ADV + 1, true, INT_REST, TGT_OPEN, 0, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);
    reset_counts();
    work_frame(f, ADV, true, INT_REST, TGT_OPEN, 0, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_stop_calls == 0);
    assert(g_last_speed > 3.9f && g_last_speed < 4.1f);

    /* Next step is active with an HR target: no usable speed, so nothing is
     * pre-rolled and the current command holds — same as at the boundary. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, ADV + 1, true, INT_ACTIVE, TGT_HR, 160, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);
    reset_counts();
    work_frame(f, ADV, true, INT_ACTIVE, TGT_HR, 160, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Short-step guard: a TIME step shorter than 2 x advance would be
     * pre-empted almost as soon as it started, so it is left alone. The guard
     * is on the step's own length, not on how much of it is left. */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 2 * ADV - 1, ADV, true, INT_ACTIVE, TGT_SPEED,
           NEXT_MMPS, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 2 * ADV, ADV, true, INT_ACTIVE, TGT_SPEED,
           NEXT_MMPS, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    /* The guard only knows TIME durations: a short DISTANCE step relies on
     * remaining_s alone and still pre-rolls. */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_DIST, 2 * ADV - 1, ADV, true, INT_ACTIVE, TGT_SPEED,
           NEXT_MMPS, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    /* FLAG_ADV_UP_ONLY: the opt-out for runners who want the whole work
     * interval on the belt. A faster next step still pre-rolls... */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 60, ADV, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS,
           ADV, true);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    /* ...but a slower one does not, and the work pace holds to the boundary. */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, NEXT_MMPS, NEXT_MMPS, INT_ACTIVE,
           DUR_TIME, 60, ADV + 1, true, INT_REST, TGT_OPEN, 0, ADV, true);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);
    reset_counts();
    frame2(f, TIMER_ON, true, TGT_SPEED, NEXT_MMPS, NEXT_MMPS, INT_ACTIVE,
           DUR_TIME, 60, ADV, true, INT_REST, TGT_OPEN, 0, ADV, true);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);
    /* Without the flag the same frame eases down early. */
    reset_counts();
    frame2(f, TIMER_ON, true, TGT_SPEED, NEXT_MMPS, NEXT_MMPS, INT_ACTIVE,
           DUR_TIME, 60, ADV, true, INT_REST, TGT_OPEN, 0, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 3.9f && g_last_speed < 4.1f);

    /* Pausing mid-pre-roll stops the belt (the timer rule outranks the
     * pre-roll), and resuming brings back the *pre-rolled* speed, because that
     * is what was latched. */
    reset_counts();
    workout_ctrl_reset();
    work_frame(f, ADV, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    reset_counts();
    frame2(f, TIMER_PAUSED, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 60, 3, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 1 && g_speed_calls == 0);

    reset_counts();
    work_frame(f, 3, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, ADV);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    /* A corrupt advance byte is clamped to ADVANCE_MAX_S (30) rather than
     * pre-empting the whole step: 30 s out still pre-rolls, 31 s out does not,
     * and the short-step guard follows the clamped value (2 x 30 = 60 s). */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 600, 31, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, 200, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    reset_counts();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 600, 30, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, 200, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 11.9f && g_last_speed < 12.1f);

    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, true, TGT_SPEED, WORK_MMPS, WORK_MMPS, INT_ACTIVE,
           DUR_TIME, 59, 30, true, INT_ACTIVE, TGT_SPEED, NEXT_MMPS, 200, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* A free run in v2 is still a free run: no step, no next, nothing early. */
    reset_counts();
    workout_ctrl_reset();
    frame2(f, TIMER_ON, false, TGT_NONE, 0, 0, INT_NONE,
           DUR_NONE, 0, REMAIN_UNKNOWN, false, INT_NONE, TGT_NONE, 0, ADV, false);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    printf("workout_ctrl: OK\n");
    return 0;
}
