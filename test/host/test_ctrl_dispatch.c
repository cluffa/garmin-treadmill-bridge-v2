#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "ctrl_dispatch.h"
#include "workout_ctrl.h"
#include "machine.h"

/* ---- machine_* stubs: record what ctrl_dispatch asked for ---- */
static float   g_last_speed;
static bool    g_speed_called;
static ftms_device_t g_devs[FTMS_MAX_DEVICES];
static int     g_ndev;
static int     g_connect_calls;
static int     g_note_manual_calls;
static int     g_note_manual_last_kind;
static float   g_note_manual_last_kmh;

/* stub — test_ctrl_dispatch does not link workout_ctrl.c */
void workout_ctrl_note_manual(int kind, float kmh) {
    g_note_manual_calls++;
    g_note_manual_last_kind = kind;
    g_note_manual_last_kmh = kmh;
}
void workout_ctrl_reset(void) {}
void workout_ctrl_tick(void) {}
void workout_ctrl_on_frame(const uint8_t *d, uint16_t len) { (void)d; (void)len; }

void   machine_start_scan(void) {}
int    machine_get_devices(ftms_device_t *out, int max) {
    int n = g_ndev < max ? g_ndev : max;
    memcpy(out, g_devs, (size_t)n * sizeof *out);
    return n;
}
void   machine_connect(const ftms_device_t *d) { (void)d; g_connect_calls++; }
bool   machine_connected(void) { return false; }
const ftms_device_t *machine_connected_device(void) { return NULL; }
bool   machine_set_speed(float kmh) { g_last_speed = kmh; g_speed_called = true; return true; }
bool   machine_set_incline(float pct) { (void)pct; return true; }
bool   machine_stop(void) { return true; }

/* ---- capture tx output ---- */
static char g_out[2048];
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

    /* valid SPEED must latch through workout_ctrl_note_manual */
    g_note_manual_calls = 0;
    dispatch("SPEED 5.5");
    assert(g_note_manual_calls == 1);
    assert(g_note_manual_last_kind == WORKOUT_CTRL_ACT_SPEED);
    assert(g_note_manual_last_kmh == 5.5f);

    /* STOP must latch through workout_ctrl_note_manual */
    g_note_manual_calls = 0;
    dispatch("STOP");
    assert(g_note_manual_calls == 1);
    assert(g_note_manual_last_kind == WORKOUT_CTRL_ACT_STOP);

    /* device name with a quote must be escaped so the JSON stays valid */
    g_ndev = 1;
    memset(&g_devs[0], 0, sizeof g_devs[0]);
    strcpy(g_devs[0].name, "Bad\"Name");
    dispatch("LIST");
    assert(strstr(g_out, "Bad\\\"Name"));

    /* ---- FIX 1: LIST with max devices, max-length escaped names must not
     * overflow the stack buffer or produce malformed JSON. ---- */
    g_ndev = FTMS_MAX_DEVICES;
    /* Craft a scenario where pos reaches 431 after the first 7 entries, then
     * an 84-byte escaped entry overflows: snprintf truncates at 80 bytes
     * but returns 84, pushing pos to 515, so the closing "]}" writes at
     * buf+515 with size (512-515) which underflows to a huge size_t. */
    for (int i = 0; i < FTMS_MAX_DEVICES; i++) {
        memset(&g_devs[i], 0, sizeof g_devs[i]);
        if (i < 7) {
            /* 12-char plain names → ~58 bytes per entry.
             * After 7 entries pos = 26 + 57 + 6*58 = 431. */
            memset(g_devs[i].name, 'A', 12);
            g_devs[i].name[12] = '\0';
        } else {
            /* 19 double-quote chars → 38 escaped chars → ~84 byte entry.
             * snprintf(buf+431, 81, …) writes 80 chars, returns 84,
             * pos = 515 → final snprintf underflows size. */
            memset(g_devs[i].name, '"', FTMS_NAME_LEN - 1);
            g_devs[i].name[FTMS_NAME_LEN - 1] = '\0';
        }
        g_devs[i].proto = MACHINE_PROTO_IFIT;
        g_devs[i].rssi = -80 + i;
    }
    dispatch("LIST");
    /* 1. Output must be NUL-terminated (would fail if snprintf overflow
     *    wrote past the buffer). */
    size_t out_len = strlen(g_out);
    assert(out_len < sizeof g_out);
    /* 2. Balanced JSON brackets (would fail if truncated mid-object). */
    int brace = 0, bracket = 0;
    for (size_t i = 0; i < out_len; i++) {
        if (g_out[i] == '{') brace++;
        if (g_out[i] == '}') brace--;
        if (g_out[i] == '[') bracket++;
        if (g_out[i] == ']') bracket--;
    }
    assert(brace == 0 && bracket == 0);

    /* ---- FIX 3: CONNECT must reject garbage with an error, not silently
     * connect to device 0. ---- */
    g_connect_calls = 0;
    dispatch("CONNECT foo");
    assert(g_connect_calls == 0);
    assert(strstr(g_out, "\"ok\":false"));

    g_connect_calls = 0;
    dispatch("CONNECT 1x");
    assert(g_connect_calls == 0);
    assert(strstr(g_out, "\"ok\":false"));

    g_connect_calls = 0;
    dispatch("CONNECT");
    assert(g_connect_calls == 0);
    assert(strstr(g_out, "error"));

    printf("ctrl_dispatch: OK\n");
    return 0;
}
