/*
 * workout_ctrl.h — treadmill control driven by the watch's workout telemetry.
 *
 * The Garmin data field no longer decides speeds. It packs the *raw* current
 * workout step (target/duration/intensity) plus the activity timer state into
 * a small binary frame and writes it to the control service's telemetry
 * characteristic (A6ED0004). This module decodes that frame and owns all the
 * control policy that used to live on the watch:
 *   - resolve the current speed target (low/high midpoint, unit conversion),
 *   - command it *on change* immediately (no fixed re-send cadence),
 *   - re-assert it on a slow keepalive in case a write was lost,
 *   - stop the belt when the activity is paused/stopped,
 *   - keep the belt moving through interval rest steps.
 *
 * Platform-agnostic: depends only on machine.h (no SoftDevice / nRF / ESP
 * includes), so both the nRF52840 and ESP32 control services share it. Each
 * board hands raw writes to workout_ctrl_on_frame() and calls
 * workout_ctrl_tick() at ~1 Hz from an existing timer.
 *
 * ---- Wire format (little-endian, WORKOUT_FRAME_LEN bytes) ------------------
 *   [0]     version           (WORKOUT_FRAME_VERSION)
 *   [1]     timerState        (Activity.TIMER_STATE_*: 0 off,1 stopped,2 paused,3 on)
 *   [2]     flags             (bit0 = a workout step is present)
 *   [3]     intensity         (Activity.WORKOUT_INTENSITY_*: 0 active,1 rest,…)
 *   [4]     targetType        (Activity.WORKOUT_STEP_TARGET_*: 0 = speed)
 *   [5..6]  targetLow         (uint16; for a speed target, mm/s)
 *   [7..8]  targetHigh        (uint16; for a speed target, mm/s)
 *   [9]     durationType      (Activity.WORKOUT_STEP_DURATION_*; informational)
 *   [10..13] durationValue    (uint32; informational)
 *   [14]    repetitionNumber  (interval rep, 0 if not an interval; informational)
 *
 * The watch resolves interval work/rest to the *current* portion using
 * WorkoutStepInfo.intensity before packing, so targetLow/High always describe
 * the step in progress. Keep this layout in sync with the data field's
 * DataFieldView.mc frame builder.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WORKOUT_FRAME_VERSION  1
#define WORKOUT_FRAME_LEN      15

/* Feed one telemetry frame from the watch. Applies any resulting command
 * change to the treadmill immediately. Silently ignores malformed frames. */
void workout_ctrl_on_frame(const uint8_t *data, uint16_t len);

/* Call at ~1 Hz from a board timer. Re-asserts the current speed command on a
 * slow keepalive (guards against a lost write); does nothing otherwise. */
void workout_ctrl_tick(void);

/* Drop any latched command (e.g. on watch disconnect) so the keepalive stops
 * re-asserting a stale target. */
void workout_ctrl_reset(void);
