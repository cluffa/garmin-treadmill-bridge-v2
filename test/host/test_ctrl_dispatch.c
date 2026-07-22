#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "ctrl_dispatch.h"
#include "machine.h"

/* ---- machine_* stubs: record what ctrl_dispatch asked for ---- */
static float   g_last_speed;
static bool    g_speed_called;
static ftms_device_t g_devs[2];
static int     g_ndev;

void   machine_start_scan(void) {}
int    machine_get_devices(ftms_device_t *out, int max) {
    int n = g_ndev < max ? g_ndev : max;
    memcpy(out, g_devs, (size_t)n * sizeof *out);
    return n;
}
void   machine_connect(const ftms_device_t *d) { (void)d; }
bool   machine_connected(void) { return false; }
const ftms_device_t *machine_connected_device(void) { return NULL; }
bool   machine_set_speed(float kmh) { g_last_speed = kmh; g_speed_called = true; return true; }
bool   machine_set_incline(float pct) { (void)pct; return true; }
bool   machine_stop(void) { return true; }

/* ---- capture tx output ---- */
static char g_out[512];
static void cap(const char *msg, void *ctx) { (void)ctx; snprintf(g_out, sizeof g_out, "%s", msg); }

static void dispatch(const char *line) { g_out[0] = '\0'; ctrl_dispatch(line, cap, NULL); }

int main(void) {
    /* valid speed reaches the machine */
    g_speed_called = false;
    dispatch("SPEED 8.5");
    assert(g_speed_called && g_last_speed == 8.5f);
    assert(strstr(g_out, "\"ok\":true"));

    /* garbage must NOT command 0 — it's rejected */
    g_speed_called = false;
    dispatch("SPEED foo");
    assert(!g_speed_called);
    assert(strstr(g_out, "bad value"));

    /* trailing garbage is rejected too */
    g_speed_called = false;
    dispatch("SPEED 8x");
    assert(!g_speed_called);

    /* device name with a quote must be escaped so the JSON stays valid */
    g_ndev = 1;
    memset(&g_devs[0], 0, sizeof g_devs[0]);
    strcpy(g_devs[0].name, "Bad\"Name");
    dispatch("LIST");
    assert(strstr(g_out, "Bad\\\"Name"));

    printf("ctrl_dispatch: OK\n");
    return 0;
}
