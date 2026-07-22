#pragma once
#include <stdbool.h>
#include "ftms_devlist.h"

/*
 * last_device — persist the last-connected treadmill in flash (FDS), the
 * nRF equivalent of the ESP32 build's NVS "ftms/last" blob.
 *
 * fds runs its writes asynchronously off SoftDevice flash events; save is
 * fire-and-forget (a lost write costs one auto-reconnect preference, not
 * data). Saves are skipped when the stored record already matches, so
 * steady-state reconnects don't wear flash.
 */

/* Synchronous: registers the fds handler and waits for init to complete.
 * Call once after the SoftDevice is enabled, before scanning starts. */
void last_device_init(void);

/* Load the saved device. Returns false if none has ever been saved. */
bool last_device_load(ftms_device_t *out);

/* Queue an async save of d as the new last-connected device. */
void last_device_save(const ftms_device_t *d);
