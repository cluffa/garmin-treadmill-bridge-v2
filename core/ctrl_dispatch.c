#include "ctrl_dispatch.h"
#include "machine.h"
#include "ftms_devlist.h"
#include "workout_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Copy src into dst escaping " and \ so an advert-supplied device name can't
 * break the JSON we emit (the phone silently drops malformed lines). */
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < cap; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[o++] = '\\';
        dst[o++] = src[i];
    }
    dst[o] = '\0';
}

/* Parse a full-token float; returns false on trailing garbage or empty input
 * so a malformed "SPEED foo" doesn't silently command 0. */
static bool parse_float(const char *s, float *out)
{
    char *end;
    float v = strtof(s, &end);
    while (*end == ' ') end++;
    if (end == s || *end != '\0') return false;
    *out = v;
    return true;
}

static void cmd_scan(ctrl_tx_fn tx, void *ctx)
{
    machine_start_scan();
    tx("{\"cmd\":\"scan\",\"ok\":true}", ctx);
}

static void cmd_list(ctrl_tx_fn tx, void *ctx)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);
    char buf[512];
    int pos = snprintf(buf, sizeof buf, "{\"cmd\":\"list\",\"devices\":[");
    for (int i = 0; i < n; i++) {
        const char *proto = devs[i].proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS";
        char name[FTMS_NAME_LEN * 2];
        json_escape(name, sizeof name, devs[i].name);
        int w = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s{\"idx\":%d,\"name\":\"%s\",\"proto\":\"%s\",\"rssi\":%d}",
                         i ? "," : "", i, name, proto, devs[i].rssi);
        /* snprintf returns what it WOULD have written, not what it wrote.
         * If the return exceeds the space remaining -- less the 2 bytes the
         * closing "]}" still needs -- this entry was truncated; drop it and
         * stop, rather than letting pos drift past the end of the buffer.
         * Reserving the 2 bytes here (rather than clamping pos afterwards)
         * is what keeps the output valid JSON: a clamp could cut the last
         * complete entry mid-token. */
        if (w < 0 || w >= (int)sizeof(buf) - pos - 2) break;
        pos += w;
    }
    /* pos is now <= sizeof(buf) - 3, so "]}" plus its NUL always fits. */
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    tx(buf, ctx);
}

static void cmd_connect(int idx, ctrl_tx_fn tx, void *ctx)
{
    ftms_device_t devs[FTMS_MAX_DEVICES];
    int n = machine_get_devices(devs, FTMS_MAX_DEVICES);
    if (idx < 0 || idx >= n) {
        tx("{\"cmd\":\"connect\",\"ok\":false,\"err\":\"bad index\"}", ctx);
        return;
    }
    machine_connect(&devs[idx]);
    tx("{\"cmd\":\"connect\",\"ok\":true}", ctx);
}

static void cmd_speed(float kmh, ctrl_tx_fn tx, void *ctx)
{
    bool ok = machine_set_speed(kmh);
    /* Latch the manual speed so the keepalive re-asserts THIS value,
     * not the last workout-step target. */
    if (ok) workout_ctrl_note_manual(WORKOUT_CTRL_ACT_SPEED, kmh);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"cmd\":\"speed\",\"ok\":%s}", ok ? "true" : "false");
    tx(buf, ctx);
}

static void cmd_incline(float pct, ctrl_tx_fn tx, void *ctx)
{
    bool ok = machine_set_incline(pct);
    char buf[64];
    snprintf(buf, sizeof buf, "{\"cmd\":\"incline\",\"ok\":%s}", ok ? "true" : "false");
    tx(buf, ctx);
}

static void cmd_stop(ctrl_tx_fn tx, void *ctx)
{
    bool ok = machine_stop();
    /* Latch the manual stop so workout_ctrl_tick() does NOT re-assert a stale
     * workout speed ~30 s later. Latched on INTENT, not on success: if the
     * stop failed the belt may still be moving, and re-commanding the old
     * workout speed is the last thing we want to do to someone who just
     * asked for a stop. */
    workout_ctrl_note_manual(WORKOUT_CTRL_ACT_STOP, 0);
    tx(ok ? "{\"cmd\":\"stop\",\"ok\":true}" : "{\"cmd\":\"stop\",\"ok\":false}", ctx);
}

static void cmd_status(ctrl_tx_fn tx, void *ctx)
{
    bool conn = machine_connected();
    const ftms_device_t *dev = machine_connected_device();
    char buf[256];
    if (!conn || !dev) {
        snprintf(buf, sizeof buf, "{\"cmd\":\"status\",\"connected\":false}");
    } else {
        char name[FTMS_NAME_LEN * 2];
        json_escape(name, sizeof name, dev->name);
        snprintf(buf, sizeof buf,
                 "{\"cmd\":\"status\",\"connected\":true,\"name\":\"%s\"}",
                 name);
    }
    tx(buf, ctx);
}

void ctrl_dispatch(const char *line, ctrl_tx_fn tx, void *ctx)
{
    /* skip leading whitespace */
    while (*line == ' ') line++;
    /* strip trailing whitespace (caller may or may not have done this) */
    char buf[256];
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' || line[len-1] == ' '))
        len--;
    if (len == 0 || len >= sizeof buf) return;
    memcpy(buf, line, len);
    buf[len] = '\0';

    if (strcmp(buf, "SCAN") == 0)         { cmd_scan(tx, ctx); return; }
    if (strcmp(buf, "LIST") == 0)         { cmd_list(tx, ctx); return; }
    if (strcmp(buf, "STATUS") == 0)       { cmd_status(tx, ctx); return; }
    if (strcmp(buf, "STOP") == 0)         { cmd_stop(tx, ctx); return; }
    if (strncmp(buf, "CONNECT ", 8) == 0) {
        char *end;
        long idx = strtol(buf + 8, &end, 10);
        while (*end == ' ') end++;
        if (end == buf + 8 || *end != '\0') {
            tx("{\"cmd\":\"connect\",\"ok\":false,\"err\":\"bad value\"}", ctx);
            return;
        }
        cmd_connect((int)idx, tx, ctx);
        return;
    }
    if (strncmp(buf, "SPEED ", 6) == 0) {
        float v;
        if (parse_float(buf + 6, &v)) cmd_speed(v, tx, ctx);
        else tx("{\"cmd\":\"speed\",\"ok\":false,\"err\":\"bad value\"}", ctx);
        return;
    }
    if (strncmp(buf, "INCLINE ", 8) == 0) {
        float v;
        if (parse_float(buf + 8, &v)) cmd_incline(v, tx, ctx);
        else tx("{\"cmd\":\"incline\",\"ok\":false,\"err\":\"bad value\"}", ctx);
        return;
    }
    tx("{\"event\":\"error\",\"msg\":\"unknown command\"}", ctx);
}
