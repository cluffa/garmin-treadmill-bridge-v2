#pragma once
/*
 * testboard.h — expansion-board test aid: OLED render + button-driven self-test.
 *
 * M2 Task 2.4.
 *
 * When compiled with TESTBOARD=0 the .c file is excluded from the build;
 * when TESTBOARD=1 it renders app_state to the SSD1306 OLED, drives the RGB
 * status LED, chirps the buzzer on state transitions, and lets the button
 * cycle through test actions including a synthetic workout-frame injector
 * that proves the brain (workout_ctrl) is observable with no radios.
 *
 * Compiled only when TESTBOARD=1 (the SRC_FILES list gates it in the
 * Makefile), so this header need not be conditional.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Lifecycle ------------------------------------------------------------- */

void testboard_init(void);

/* Called at ~5 Hz from an app_timer. Composes OLED lines from app_state(),
 * updates the status LED, and chirps the buzzer on state transitions.
 *
 * IMPORTANT: this runs at IRQ priority 6 (same as SD_EVT_IRQn) — it must
 * NOT do blocking I2C. The OLED framebuffer is composed here but the actual
 * hardware flush is deferred to testboard_process(). */
void testboard_render_tick(void);

/*
 * Called from the main loop (not IRQ context). Flushes the OLED framebuffer
 * to hardware via blocking I2C when a render tick has marked it dirty.
 * Must be polled regularly; if not called the display will not update but
 * the device will not crash or hang.
 */
void testboard_process(void);

/*
 * Weak radio-action stubs that M3 (ble_central) will override with real
 * implementations. Called by the button short-press action cycler.
 */
void testboard_action_scan(void);
void testboard_action_connect_next(void);
void testboard_action_stop(void);

#ifdef __cplusplus
}
#endif
