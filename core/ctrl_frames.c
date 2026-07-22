#include "ctrl_frames.h"
#include <string.h>

/* Copy src into dst truncated to max chars, always NUL-terminated; returns
 * the number of bytes written including the NUL. */
static int put_name(uint8_t *dst, const char *src, int max)
{
    int n = 0;
    if (src != NULL) {
        while (n < max && src[n] != '\0') {
            dst[n] = (uint8_t)src[n];
            n++;
        }
    }
    dst[n] = '\0';
    return n + 1;
}

int ctrl_frame_device(uint8_t *out, uint8_t idx, const ftms_device_t *d,
                      uint8_t flags)
{
    out[0] = 'D';
    out[1] = idx;
    out[2] = (uint8_t)d->rssi;
    out[3] = d->proto;
    out[4] = flags;
    return 5 + put_name(&out[5], d->name, CTRL_DEV_NAME_MAX);
}

int ctrl_frame_list_end(uint8_t *out, uint8_t count)
{
    out[0] = 'E';
    out[1] = count;
    return 2;
}

int ctrl_frame_status(uint8_t *out, bool connected, uint8_t proto,
                      const char *name)
{
    out[0] = 'S';
    out[1] = connected ? 1 : 0;
    out[2] = proto;
    return 3 + put_name(&out[3], name, CTRL_STATUS_NAME_MAX);
}
