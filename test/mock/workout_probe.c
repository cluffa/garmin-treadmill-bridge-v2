/*
 * workout_probe.c — expose core/workout_ctrl.c to mock_bridge.py via ctypes.
 *
 * The mock bridge reports what the *real* bridge would do with each frame the
 * watch sends. That only means anything if the policy is the firmware's own, so
 * this file supplies the machine_* stubs workout_ctrl.c links against, records
 * which command it issued, and exposes a scalars-only ABI (no structs, so there
 * is no layout to keep in sync with the Python side).
 *
 * Lives in test/mock/, not core/ — the core/ purity invariant is untouched.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "machine.h"
#include "workout_ctrl.h"

#define PROBE_ACT_NONE  0
#define PROBE_ACT_SPEED 1
#define PROBE_ACT_STOP  2

static int   g_act;
static float g_last_speed;

/* ---- machine_* stubs: record what workout_ctrl commanded ---- */
void  machine_start_scan(void) {}
int   machine_get_devices(ftms_device_t *o, int m) { (void)o; (void)m; return 0; }
void  machine_connect(const ftms_device_t *d) { (void)d; }
bool  machine_connected(void) { return true; }
const ftms_device_t *machine_connected_device(void) { return NULL; }
bool  machine_set_incline(float p) { (void)p; return true; }
bool  machine_set_speed(float kmh) { g_act = PROBE_ACT_SPEED; g_last_speed = kmh; return true; }
bool  machine_stop(void) { g_act = PROBE_ACT_STOP; return true; }

/* ---- probe ABI ---- */

void probe_reset(void)
{
    workout_ctrl_reset();
    g_act = PROBE_ACT_NONE;
    g_last_speed = 0.0f;
}

/* Feed one frame. Returns the command actually issued to the machine, so a
 * deduplicated frame correctly reports PROBE_ACT_NONE. */
int probe_feed(const uint8_t *buf, uint16_t len)
{
    g_act = PROBE_ACT_NONE;
    workout_ctrl_on_frame(buf, len);
    return g_act;
}

/* One 1 Hz tick. Returns PROBE_ACT_SPEED when the keepalive re-asserted. */
int probe_tick(void)
{
    g_act = PROBE_ACT_NONE;
    workout_ctrl_tick();
    return g_act;
}

float probe_last_speed(void) { return g_last_speed; }
