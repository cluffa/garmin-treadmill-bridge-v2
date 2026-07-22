/*
 * machine_stubs.c — minimal dummy implementations of the machine.h facade.
 *
 * These are linked in now so that ctrl_dispatch compiles and links before the
 * real BLE central layer is built (M3). All functions return "not connected"
 * or "no devices" so the USB console can respond to STATUS/LIST etc. with
 * sensible defaults rather than linker errors.
 *
 * When TESTBOARD=1, machine_set_speed() / machine_stop() forward the resolved
 * target to app_state()->resolved_target_mps so the OLED renders the output
 * of workout_ctrl (critical for M2's "brain observable with no radios" gate).
 *
 * Replace this file with the real ble_central.c + machine glue in Milestone M3.
 */

#include "machine.h"
#include "app_state.h"
#include <string.h>

/* ---- Stubs ---------------------------------------------------------------- */

void machine_set_addr_type(uint8_t addr_type) { (void)addr_type; }

void machine_set_data_cb(machine_state_cb cb) { (void)cb; }

void machine_start_scan(void) {}

int  machine_get_devices(ftms_device_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

void machine_connect(const ftms_device_t *dev) { (void)dev; }

void machine_try_last(void) {}

bool machine_connected(void) { return false; }

const ftms_device_t *machine_connected_device(void) { return NULL; }

bool machine_connecting(void) { return false; }

int8_t machine_conn_rssi(void) { return 0; }

bool machine_saved_device(ftms_device_t *out)
{
    (void)out;
    return false;
}

void machine_set_link_cb(void (*cb)(bool connected)) { (void)cb; }

bool machine_set_speed(float kmh)
{
    /* Store the resolved target so the testboard OLED (and later, other
     * modules) can read back what workout_ctrl commanded. */
    app_state()->resolved_target_mps = kmh / 3.6f;
    return false;
}

bool machine_set_incline(float pct)
{
    (void)pct;
    return false;
}

bool machine_stop(void)
{
    app_state()->resolved_target_mps = 0.0f;
    return false;
}
