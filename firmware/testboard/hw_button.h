#pragma once
/*
 * hw_button.h — debounced button on the expansion board.
 *
 * Pin source: Seeed XIAO expansion board — user button on D1.
 *   PIN_BUTTON   P0.03  (defined in board_pins.h), pulled high, active low.
 *
 * The button is debounced (app_button, 50 ms) and fires a short-press
 * callback immediately on release.  A long-press (>= 800 ms hold, detected
 * via app_timer) fires a separate callback.
 *
 * M2 Task 2.2.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Initialise button with callbacks ---------------------------------------
 *
 * Both callbacks are optional (pass NULL to ignore that action).
 *
 * on_short  — fired on every short (< 800 ms) press & release
 * on_long   — fired on every long  (>= 800 ms) hold, after the 800 ms
 *             threshold is reached (fires once per hold, not on release)
 */
void hw_button_init(void (*on_short)(void), void (*on_long)(void));

#ifdef __cplusplus
}
#endif
