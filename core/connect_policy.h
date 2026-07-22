#pragma once
#include <stdint.h>
#include "ftms_devlist.h"

/*
 * connect_policy — pure auto-connect chooser for the treadmill link.
 *
 * Policy (matches the user-facing contract):
 *   1. The saved (last-connected) device wins the moment it appears in the
 *      scan list, at any time.
 *   2. If a saved device exists but hasn't been seen yet, hold out
 *      CONNECT_POLICY_SAVED_WAIT_MS before settling for the best available
 *      (it may still be powering up / out of range).
 *   3. With no saved device, collect adverts for CONNECT_POLICY_PICK_WINDOW_MS
 *      and then pick the strongest RSSI — the closest treadmill. RSSI ties go
 *      to the earliest list entry, i.e. the first one found.
 *
 * Pure logic, host-tested; the platform calls it on every advert and on a
 * periodic tick while scanning and connects to the returned index.
 */

#define CONNECT_POLICY_PICK_WINDOW_MS  6000u
#define CONNECT_POLICY_SAVED_WAIT_MS   15000u

/* Returns the index in list[0..n) to connect to, or -1 to keep scanning.
 * `saved` is the persisted last-connected device, or NULL if none exists.
 * `ms_since_scan_start` is wall time since this scan began. */
int connect_policy_choose(const ftms_device_t *list, int n,
                          const ftms_device_t *saved,
                          uint32_t ms_since_scan_start);
