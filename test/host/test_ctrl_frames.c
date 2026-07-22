/* Host tests for ctrl_frames — the compact (≤20-byte) binary frames the
 * bridge notifies to the watch over the control service's response
 * characteristic. Garmin CIQ keeps the ATT MTU at 23, so every frame must
 * fit a 20-byte notification payload. */
#include "ctrl_frames.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_device_frame_layout(void)
{
    ftms_device_t d;
    memset(&d, 0, sizeof d);
    d.rssi = -63;
    d.proto = MACHINE_PROTO_IFIT;
    strcpy(d.name, "I_TL");

    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_device(buf, 4, &d, CTRL_DEV_FLAG_CONNECTED);
    assert(n > 0 && n <= CTRL_FRAME_MAX);
    assert(buf[0] == 'D');
    assert(buf[1] == 4);
    assert((int8_t)buf[2] == -63);
    assert(buf[3] == MACHINE_PROTO_IFIT);
    assert(buf[4] == CTRL_DEV_FLAG_CONNECTED);
    assert(memcmp(&buf[5], "I_TL", 4) == 0);
    assert(buf[9] == '\0');   /* name NUL-terminated inside the frame */
}

static void test_device_frame_truncates_long_name(void)
{
    ftms_device_t d;
    memset(&d, 0, sizeof d);
    strcpy(d.name, "1234567890123456789");   /* FTMS_NAME_LEN-1 = 19 chars */

    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_device(buf, 0, &d, 0);
    assert(n <= CTRL_FRAME_MAX);
    /* Name payload is at most CTRL_DEV_NAME_MAX chars and NUL-terminated. */
    assert(memcmp(&buf[5], "12345678901234", CTRL_DEV_NAME_MAX) == 0);
    assert(buf[5 + CTRL_DEV_NAME_MAX] == '\0');
    assert(n == 5 + CTRL_DEV_NAME_MAX + 1);
}

static void test_list_end_frame(void)
{
    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_list_end(buf, 3);
    assert(n == 2);
    assert(buf[0] == 'E');
    assert(buf[1] == 3);
}

static void test_status_frame(void)
{
    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_status(buf, true, MACHINE_PROTO_FTMS, "TreadmillXYZ");
    assert(n <= CTRL_FRAME_MAX);
    assert(buf[0] == 'S');
    assert(buf[1] == 1);
    assert(buf[2] == MACHINE_PROTO_FTMS);
    assert(memcmp(&buf[3], "TreadmillXYZ", 12) == 0);
    assert(buf[15] == '\0');

    n = ctrl_frame_status(buf, false, 0, NULL);
    assert(n >= 3);
    assert(buf[0] == 'S');
    assert(buf[1] == 0);
}

static void test_status_frame_truncates_long_name(void)
{
    uint8_t buf[CTRL_FRAME_MAX];
    int n = ctrl_frame_status(buf, true, MACHINE_PROTO_IFIT,
                              "1234567890123456789");
    assert(n <= CTRL_FRAME_MAX);
    assert(buf[3 + CTRL_STATUS_NAME_MAX] == '\0');
}

int main(void)
{
    test_device_frame_layout();
    test_device_frame_truncates_long_name();
    test_list_end_frame();
    test_status_frame();
    test_status_frame_truncates_long_name();
    printf("test_ctrl_frames: all tests passed\n");
    return 0;
}
