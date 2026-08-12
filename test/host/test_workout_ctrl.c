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
#define INT_ACTIVE 0
#define INT_REST   1
#define INT_NONE   0xFF   /* watch sentinel: no step resolved */

/* timer_state, has_step, target_type, low/high in mm/s, intensity */
static void frame_i(uint8_t *f, uint8_t timer, bool has_step, uint8_t target,
                    uint16_t low_mmps, uint16_t high_mmps, uint8_t intensity)
{
    memset(f, 0, WORKOUT_FRAME_LEN);
    f[0] = WORKOUT_FRAME_VERSION;
    f[1] = timer;
    f[2] = has_step ? 0x01 : 0x00;
    f[3] = intensity;
    f[4] = target;
    put_u16(f + 5, low_mmps);
    put_u16(f + 7, high_mmps);
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
    workout_ctrl_on_frame(f, WORKOUT_FRAME_LEN - 1);
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

    /* ---- difficulty scaling ------------------------------------------------
     *
     * One adjustment on one interval scales every later interval. Each block
     * begins with a TIMER_OFF frame, which is the only thing that resets the
     * scale (workout_ctrl_reset deliberately does not — see workout_ctrl.h).
     */

    /* A manual SPEED override learns the ratio, and the NEXT step is scaled. */
    reset_counts();
    workout_ctrl_reset();
    frame(f, TIMER_OFF, false, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(workout_ctrl_scale() > 0.99f && workout_ctrl_scale() < 1.01f);

    frame(f, TIMER_ON, true, TGT_SPEED, 1667, 1667);   /* 6.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_last_speed > 5.9f && g_last_speed < 6.1f);
    assert(workout_ctrl_base_kmh() > 5.9f && workout_ctrl_base_kmh() < 6.1f);

    /* User drops it 10%, to 5.4. */
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_SPEED, 5.4f);
    assert(workout_ctrl_scale() > 0.895f && workout_ctrl_scale() < 0.905f);

    /* Next interval is authored at 7.0 → must be commanded at 6.3. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 1944, 1944);   /* 7.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    assert(g_last_speed > 6.25f && g_last_speed < 6.35f);

    /* A second adjustment compounds off the AUTHORED value, not the commanded
     * one: 5.67 / 7.0 = 0.81. */
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_SPEED, 5.67f);
    assert(workout_ctrl_scale() > 0.805f && workout_ctrl_scale() < 0.815f);

    /* Rest steps scale too — an easier session eases the walk breaks as well. */
    reset_counts();
    frame_i(f, TIMER_ON, false, TGT_OPEN, 0, 0, INT_REST);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    assert(g_last_speed > 3.19f && g_last_speed < 3.29f);   /* 4.0 * 0.81 */

    /* Pause/stop preserve the scale; only TIMER_OFF clears it. */
    reset_counts();
    frame(f, TIMER_PAUSED, true, TGT_SPEED, 1944, 1944);
    workout_ctrl_on_frame(f, sizeof f);
    assert(workout_ctrl_scale() < 0.9f);
    workout_ctrl_reset();
    assert(workout_ctrl_scale() < 0.9f);
    frame(f, TIMER_OFF, false, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(workout_ctrl_scale() > 0.99f && workout_ctrl_scale() < 1.01f);

    /* A manual STOP is not a difficulty opinion. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);   /* 10.0 */
    workout_ctrl_on_frame(f, sizeof f);
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_STOP, 0);
    assert(workout_ctrl_scale() > 0.99f && workout_ctrl_scale() < 1.01f);

    /* ---- belt-divergence learning (the treadmill's own speed buttons) ---- */

    reset_counts();
    workout_ctrl_reset();
    frame(f, TIMER_OFF, false, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);   /* 10.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_last_speed > 9.9f && g_last_speed < 10.1f);

    /* Ramping up: the belt is nowhere near 10 yet, and that must not be read
     * as an adjustment however long it lasts — the detector is not armed. */
    for (int i = 0; i < 20; i++) workout_ctrl_note_belt(4.0f + 0.2f * i);
    assert(workout_ctrl_scale() > 0.99f);

    /* Belt reaches the commanded speed → detector arms. */
    workout_ctrl_note_belt(10.0f);
    assert(workout_ctrl_scale() > 0.99f);

    /* User presses speed-down on the console. One sample is not enough. */
    workout_ctrl_note_belt(9.0f);
    workout_ctrl_note_belt(9.0f);
    assert(workout_ctrl_scale() > 0.99f);

    /* Keepalive must not stomp the change while it is mid-confirmation. */
    reset_counts();
    for (int i = 0; i < 40; i++) workout_ctrl_tick();
    assert(g_speed_calls == 0);

    /* Third consecutive sample → adopted: 9.0 / 10.0 = 0.90. */
    workout_ctrl_note_belt(9.0f);
    assert(workout_ctrl_scale() > 0.895f && workout_ctrl_scale() < 0.905f);

    /* No command is issued for it (the belt is already there), but the
     * keepalive now re-asserts the user's 9.0, not the authored 10.0. */
    assert(g_speed_calls == 0);
    for (int i = 0; i < 30; i++) workout_ctrl_tick();
    assert(g_speed_calls == 1);
    assert(g_last_speed > 8.9f && g_last_speed < 9.1f);

    /* And the next authored interval carries the same 10% cut: 8.0 → 7.2. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2222, 2222);   /* 8.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1);
    assert(g_last_speed > 7.15f && g_last_speed < 7.25f);

    /* Ordinary tracking error must NOT be learned: 8.0 commanded → 7.2, and a
     * belt sitting at 7.1 is inside the deadband. */
    workout_ctrl_note_belt(7.2f);            /* arm */
    for (int i = 0; i < 10; i++) workout_ctrl_note_belt(7.1f);
    assert(workout_ctrl_scale() > 0.895f && workout_ctrl_scale() < 0.905f);

    /* THE DANGEROUS ONE: safety key pulled → belt reads ~0. The ratio is out
     * of band, so it is discarded outright. Clamping it into range would
     * restart the next interval at half pace with nobody having asked. */
    for (int i = 0; i < 10; i++) workout_ctrl_note_belt(0.0f);
    assert(workout_ctrl_scale() > 0.895f && workout_ctrl_scale() < 0.905f);
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 2778, 2778);   /* 10.0 authored */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_last_speed > 8.9f && g_last_speed < 9.1f);  /* 9.0, not 5.0 */

    /* A belt feed with nothing latched is a no-op, not a crash. */
    workout_ctrl_reset();
    workout_ctrl_note_belt(8.0f);

    printf("workout_ctrl: OK\n");
    return 0;
}
