#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * ifit_fsm — the iFit/NordicTrack keepalive + control FSM, extracted from
 * the ESP32 machine_ifit.c so the nRF port drives the exact same
 * hardware-verified frame sequence.
 *
 * Pure logic, no BLE calls: the platform owns a 500 ms timer and calls
 * ifit_fsm_tick() each tick; the FSM emits the frame(s) due for that tick
 * through the write callback (1 poll/init frame, plus control injections).
 *
 * Timing contract (hardware-verified on the NordicTrack 6.5S, see CLAUDE.md):
 *  - 18-command init sequence first (~9 s at 500 ms/tick), then the 6-phase
 *    poll cycle repeats.
 *  - Speed/incline control frames are ONLY accepted right after the phase-2
 *    poll frame; the treadmill silently ignores writes outside that slot.
 *  - From a stopped belt, forceSpeed does nothing: the 5-frame START_SEQ
 *    must go out right after the phase-5 poll frame; the pending speed is
 *    then applied on a later phase 2 once the belt reports movement.
 */

typedef void (*ifit_write_fn)(const uint8_t *frame, size_t len, void *ctx);

/* Restart from the top of the init sequence (call on connect). */
void ifit_fsm_reset(void);

/* Emit this tick's frame(s). Call every ~500 ms while connected. */
void ifit_fsm_tick(ifit_write_fn write, void *ctx);

/* Queue control for injection at the correct poll phase. */
void ifit_fsm_request_speed(float kmh);
void ifit_fsm_request_incline(float pct);
void ifit_fsm_request_stop(void);

/* Feed the last decoded belt speed so the FSM knows moving vs stopped. */
void ifit_fsm_note_speed(float kmh);
