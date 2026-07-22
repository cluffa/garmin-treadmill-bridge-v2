#include "ctrl_dispatch.h"
#include "machine.h"
#include "ftms_devlist.h"

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
    for (int i = 0; i < n && pos < (int)sizeof(buf) - 80; i++) {
        const char *proto = devs[i].proto == MACHINE_PROTO_IFIT ? "iFit" : "FTMS";
        char name[FTMS_NAME_LEN * 2];
        json_escape(name, sizeof name, devs[i].name);
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%s{\"idx\":%d,\"name\":\"%s\",\"proto\":\"%s\",\"rssi\":%d}",
                        i ? "," : "", i, name, proto, devs[i].rssi);
    }
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
    if (strncmp(buf, "CONNECT ", 8) == 0) { cmd_connect(atoi(buf + 8), tx, ctx); return; }
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
