#pragma once
/*
 * app_state.h — single source of truth for device-wide state.
 *
 * M2 Task 2.1: one singleton struct aggregating treadmill belt readings,
 * connection status, and resolved workout-control output.  All radio /
 * testboard modules read and update this shared struct; no other module
 * owns private copies of what belongs here.
 *
 * Platform-agnostic types (treadmill_state_t) are pulled from core/model.h;
 * firmware-only glue (link_state_t, app_state_t) is defined here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "model.h"   /* treadmill_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Connection state (BLE central treadmill link) -------------------------- */
typedef enum {
    LINK_DOWN,
    LINK_SCANNING,
    LINK_CONNECTING,
    LINK_UP,
} link_state_t;

/* ---- Aggregate device state ------------------------------------------------- */
typedef struct {
    treadmill_state_t treadmill;          /* latest parsed belt state */
    link_state_t      central_link;       /* treadmill BLE central state */
    char              treadmill_name[24]; /* central connected-device name */
    bool              watch_connected;    /* BLE peripheral (ctrl svc) link */
    bool              ant_broadcasting;   /* ANT SDM footpod is active */
    float             resolved_target_mps;/* last target workout_ctrl commanded */
    uint32_t          last_fault_code;    /* 0 = no fault */
} app_state_t;

/* ---- Singleton access ------------------------------------------------------- */
app_state_t *app_state(void);            /* valid after app_state_init() */

/* ---- Lifecycle (called once from main) -------------------------------------- */
void app_state_init(void);

/* ---- Fault latch ------------------------------------------------------------ */
void app_state_set_fault(uint32_t code);

#ifdef __cplusplus
}
#endif
