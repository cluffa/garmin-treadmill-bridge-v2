#pragma once
/*
 * hw_buzzer.h — passive piezo buzzer, PWM-driven tone.
 *
 * Pin source: Seeed XIAO expansion board — passive buzzer on D3/A3.
 *   PIN_BUZZER  P0.02  (defined in board_pins.h)
 *
 * M2 Task 2.2.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Initialise PWM peripheral ---------------------------------------------- */
void hw_buzzer_init(void);

/* ---- Emit a tone for a given duration ----------------------------------------
 *
 * freq_hz  — tone frequency in Hz (0 = silence, stops any active chirp)
 * ms       — duration in milliseconds (0 = fire-and-forget, caller must
 *            stop manually with a second call setting freq_hz=0)
 *
 * This is a non-blocking, one-shot interface: the caller starts a chirp
 * and a one-shot app_timer stops it after `ms`.  Only one chirp is active
 * at a time; a new call cancels the previous one.
 */
void hw_buzzer_chirp(uint16_t freq_hz, uint16_t ms);

#ifdef __cplusplus
}
#endif
