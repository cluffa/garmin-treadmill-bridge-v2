#pragma once
/*
 * ble_ctrl_svc.h — S340 BLE peripheral GATT server for the watch control link.
 *
 * Exposes the A6ED vendor service with three characteristics so a Garmin
 * watch (picker app or data field) can send ctrl commands and workout
 * telemetry, and receive compact status/device-list frames.
 *
 * Public API:
 *   ble_ctrl_svc_init()           — register service, configure advertising
 *   ble_ctrl_svc_advertise_start()— begin connectable advertising
 *   ble_ctrl_svc_notify()         — push one frame to the TX queue
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ble_ctrl_svc_init(void);
void ble_ctrl_svc_advertise_start(void);
void ble_ctrl_svc_notify(const uint8_t *frame, uint16_t len);

#ifdef __cplusplus
}
#endif
