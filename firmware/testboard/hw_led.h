#pragma once
/*
 * hw_led.h — onboard RGB LED driver (active-low).
 *
 * Pin map source: Seeed XIAO nRF52840 schematic / wiki
 *   https://wiki.seeedstudio.com/XIAO_BLE/
 *   Red   P0.26, Green P0.30, Blue  P0.06 — active low (LEDS_ACTIVE_STATE = 0).
 *
 * M2 Task 2.2.
 */

#include <stdbool.h>
#include "app_state.h"  /* link_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Raw control ------------------------------------------------------------ */
void hw_led_set(bool r, bool g, bool b);

/* ---- High-level status -------------------------------------------------------
 *
 * Maps connection/fault state to a colour pattern:
 *   central link         watch  fault   LED colour
 *   LINK_DOWN            -      -       off
 *   LINK_SCANNING        -      -       blue blink  (fast)
 *   LINK_CONNECTING      -      -       yellow blink (blue+green fast)
 *   LINK_UP              no     -       green        (solid)
 *   LINK_UP              yes    -       cyan         (green+blue solid)
 *   any                  any    yes     red          (override)
 */
void hw_led_status(link_state_t central, bool watch, bool fault);

#ifdef __cplusplus
}
#endif
