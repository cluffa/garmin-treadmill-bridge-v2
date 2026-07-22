#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "machine_adapter.h"
#include "ftms_devlist.h"

/* Unified machine facade: one scan that auto-detects both FTMS (0x1826) and
 * iFit (0x1533) treadmills into a single device list, and dispatches connect /
 * status to the right adapter by each device's protocol. Drop-in for what ui.c
 * used to call on machine_ftms directly. */
void   machine_set_addr_type(uint8_t addr_type);
void   machine_set_data_cb(machine_state_cb cb);
void   machine_start_scan(void);
int    machine_get_devices(ftms_device_t *out, int max);
void   machine_connect(const ftms_device_t *dev);
void   machine_try_last(void);   /* reconnect to the saved device, else scan */
bool   machine_connected(void);
const ftms_device_t *machine_connected_device(void);
bool   machine_connecting(void);
int8_t machine_conn_rssi(void);

/* Copy the NVS-persisted last-connected device into *out. False if none. */
bool machine_saved_device(ftms_device_t *out);

/* Register a callback fired when the treadmill link comes up or goes down
 * (used by ctrl_svc to push an unsolicited 'S' status frame). */
void machine_set_link_cb(void (*cb)(bool connected));

/* Write treadmill speed/incline via FTMS Control Point. */
bool machine_set_speed(float kmh);
bool machine_set_incline(float pct);
bool machine_stop(void);
