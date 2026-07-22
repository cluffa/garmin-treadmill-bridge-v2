#pragma once
/*
 * ble_central.h — S340 BLE central / GATT client for the treadmill link.
 *
 * Scans for FTMS (0x1826) and iFit (0x1533) treadmills, connects per
 * connect_policy, subscribes to treadmill data notifications, pumps frames
 * into core/ftms_parse or core/ifit_parse, and owns the control writes
 * (FTMS Fitness Machine Control Point / iFit phased keepalive via ifit_fsm).
 *
 * Public API:
 *   ble_central_init()          — register scan module, load last device, init timers
 *   ble_central_scan_start()    — (re)start scanning; connect_policy decides when to connect
 *   ble_central_connect(int idx)— connect to the scan-list entry at idx
 *   ble_central_set_speed()     — command belt speed in m/s
 *   ble_central_disconnect()    — tear down the treadmill link
 *
 * Also implements the machine.h facade (replaces machine_stubs.c).
 */

void ble_central_init(void);
void ble_central_scan_start(void);
void ble_central_connect(int idx);
void ble_central_set_speed(float mps);
void ble_central_disconnect(void);
