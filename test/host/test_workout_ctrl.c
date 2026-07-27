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

/* timer_state, has_step, target_type, low/high in mm/s */
static void frame(uint8_t *f, uint8_t timer, bool has_step, uint8_t target,
                  uint16_t low_mmps, uint16_t high_mmps)
{
    memset(f, 0, WORKOUT_FRAME_LEN);
    f[0] = WORKOUT_FRAME_VERSION;
    f[1] = timer;
    f[2] = has_step ? 0x01 : 0x00;
    f[4] = target;
    put_u16(f + 5, low_mmps);
    put_u16(f + 7, high_mmps);
}

#define TIMER_ON 3
#define TIMER_PAUSED 2
#define TIMER_STOPPED 1
#define TIMER_OFF 0
#define TGT_SPEED 0
#define TGT_HR 1
#define TGT_OPEN 2

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

    /* Rest step with an OPEN target (no speed) → hold; belt keeps moving
     * (no stop, no new speed), and keepalive still re-asserts the held speed. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_stop_calls == 0 && g_speed_calls == 0);
    for (int i = 0; i < 30; i++) workout_ctrl_tick();
    assert(g_speed_calls == 1);          /* still holding the last work speed */
    assert(g_last_speed > 8.4f && g_last_speed < 8.6f);

    /* Rest step WITH a slower speed target → belt drops to rest pace (moving). */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_SPEED, 1389, 1389);   /* ~5.0 km/h */
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 1 && g_stop_calls == 0);
    assert(g_last_speed > 4.9f && g_last_speed < 5.1f);

    /* Free run (no step) while running → leave the belt alone. */
    reset_counts();
    frame(f, TIMER_ON, false, TGT_OPEN, 0, 0);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

    /* Non-speed target (HR) while running → hold, no command. */
    reset_counts();
    frame(f, TIMER_ON, true, TGT_HR, 150, 160);
    workout_ctrl_on_frame(f, sizeof f);
    assert(g_speed_calls == 0 && g_stop_calls == 0);

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

    printf("workout_ctrl: OK\n");
    return 0;
}
