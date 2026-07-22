#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ftms_devlist.h"

/*
 * ctrl_frames — compact binary frames notified to the watch over the control
 * service's response characteristic.
 *
 * Garmin Connect IQ keeps the ATT MTU at the default 23, so every frame must
 * fit one 20-byte notification. Names are truncated to fit; the JSON replies
 * ctrl_dispatch produces stay on RTT/NUS for the debug paths.
 *
 * Frames (byte 0 is the type tag):
 *   'D' idx rssi proto flags name…\0   one scan-list device (reply to LIST)
 *   'E' count                          end of list          (reply to LIST)
 *   'S' connected proto name…\0        treadmill link state (reply to STATUS)
 */

#define CTRL_FRAME_MAX        20
#define CTRL_DEV_NAME_MAX     14   /* 'D': 5 header + 14 name + NUL = 20 */
#define CTRL_STATUS_NAME_MAX  16   /* 'S': 3 header + 16 name + NUL = 20 */

#define CTRL_DEV_FLAG_CONNECTED 0x01  /* currently connected to this device */
#define CTRL_DEV_FLAG_SAVED     0x02  /* this is the persisted last device  */

/* Each builder writes <= CTRL_FRAME_MAX bytes into out, returns the length. */
int ctrl_frame_device(uint8_t *out, uint8_t idx, const ftms_device_t *d,
                      uint8_t flags);
int ctrl_frame_list_end(uint8_t *out, uint8_t count);
int ctrl_frame_status(uint8_t *out, bool connected, uint8_t proto,
                      const char *name);
