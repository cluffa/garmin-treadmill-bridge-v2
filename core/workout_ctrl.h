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
 *   - drop the belt to a walk on interval rest steps that carry no speed
 *     target of their own (they arrive with an OPEN target),
 *   - command the *next* step's speed advance_s seconds before the boundary
 *     (wire format v2) so the belt has finished ramping when the watch's step
 *     starts.
 *
 * Platform-agnostic: depends only on machine.h (no SoftDevice / nRF / ESP
 * includes), so both the nRF52840 and ESP32 control services share it. Each
 * board hands raw writes to workout_ctrl_on_frame() and calls
 * workout_ctrl_tick() at ~1 Hz from an existing timer.
 *
 * ---- Wire format v2 (little-endian, WORKOUT_FRAME_LEN bytes) ---------------
 *   [0]     version           (WORKOUT_FRAME_VERSION)
 *   [1]     timerState        (Activity.TIMER_STATE_*: 0 off,1 stopped,2 paused,3 on)
 *   [2]     flags             (bit0 = a workout step is present; bits 1-4 are
 *                              watch-side diagnostics the bridge ignores:
 *                              0x02 pack threw, 0x04 no step resolved,
 *                              0x08 step resolved but targetType missing,
 *                              0x10 throw was in the duration-value region;
 *                              bit5 (0x20) = the next step resolved and bytes
 *                              15-18 are valid; bit6 (0x40) = pre-roll only
 *                              speed *increases*, a user setting)
 *   [3]     intensity         (Activity.WORKOUT_INTENSITY_*: 0 active,1 rest,…;
 *                              0xFF when no step resolved. The bridge acts on
 *                              this: a rest step with no usable speed target
 *                              is commanded to REST_SPEED_KMH rather than
 *                              holding the work-interval speed.)
 *   [4]     targetType        (Activity.WORKOUT_STEP_TARGET_*: 0 = speed)
 *   [5..6]  targetLow         (uint16; for a speed target, mm/s)
 *   [7..8]  targetHigh        (uint16; for a speed target, mm/s)
 *   [9]     durationType      (Activity.WORKOUT_STEP_DURATION_*; 0 = time.
 *                              Read by the short-step guard below)
 *   [10..11] durationValue    (uint16; seconds for a TIME duration, metres for
 *                              DISTANCE, raw otherwise. Was a uint32 in v1;
 *                              nothing in a treadmill workout needs more than
 *                              65535, and the two freed bytes became [12..13])
 *   [12..13] remaining_s      (uint16; seconds left in the current step.
 *                              0xFFFF = unknown, 0xFFFE = "far" — the watch
 *                              only puts the exact value on the wire near the
 *                              boundary so a quiet step is not a 1 Hz stream)
 *   [14]    repetitionNumber  (interval rep, 0 if not an interval; informational)
 *   [15]    nextIntensity     (0xFF when there is no next step)
 *   [16]    nextTargetType    (0xFF when there is no next step)
 *   [17..18] nextTarget       (uint16 mm/s; the next step's speed range already
 *                              resolved to its midpoint by the watch. 0 = none)
 *   [19]    advance_s         (seconds to command the next step's speed *before*
 *                              the boundary, so the belt has finished ramping
 *                              when the watch's step starts. 0 = feature off;
 *                              clamped to ADVANCE_MAX_S here as well as on the
 *                              watch, so a corrupt byte cannot pre-roll a whole
 *                              step)
 *
 * ---- Wire format v1 (WORKOUT_FRAME_LEN_V1 bytes) ---------------------------
 * Bytes 0-9 are identical. [10..13] is durationValue as a uint32 and the frame
 * ends at [14]. workout_ctrl_on_frame() still accepts v1 so an older data-field
 * build keeps working against new firmware; a v1 frame simply carries no next
 * step, no remaining time and no advance, which decodes to exactly the v1
 * behaviour (command at the boundary).
 *
 * The watch resolves interval work/rest to the *current* portion using
 * WorkoutStepInfo.intensity before packing, so targetLow/High always describe
 * the step in progress. Keep this layout in sync with the data field's
 * DataFieldView.mc frame builder (test/check_frame_contract.py asserts the
 * version and length agree across C, Monkey C and the mock decoder).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define WORKOUT_FRAME_VERSION  2
#define WORKOUT_FRAME_LEN      20

/* v1 frames (15 bytes) are still accepted — see the layout note above. */
#define WORKOUT_FRAME_LEN_V1   15

/* Feed one telemetry frame from the watch. Applies any resulting command
 * change to the treadmill immediately. Silently ignores malformed frames. */
void workout_ctrl_on_frame(const uint8_t *data, uint16_t len);

/* Call at ~1 Hz from a board timer. Re-asserts the current speed command on a
 * slow keepalive (guards against a lost write); does nothing otherwise. */
void workout_ctrl_tick(void);

/* Drop any latched command (e.g. on watch disconnect) so the keepalive stops
 * re-asserting a stale target. */
void workout_ctrl_reset(void);

/* Action kinds for workout_ctrl_note_manual. These are this header's own
 * public constants — workout_ctrl.c translates them to the internal
 * action_kind_t explicitly, so they are deliberately NOT required to track
 * that enum's values. */
#define WORKOUT_CTRL_ACT_SPEED 1
#define WORKOUT_CTRL_ACT_STOP  2

/* Record a manual control command (from the watch's SPEED/STOP ctrl-svc
 * grammar) as workout_ctrl's own state so the keepalive re-asserts the
 * *manual* value rather than the last workout-step target. Without this,
 * cmd_stop()/cmd_speed() in ctrl_dispatch update the machine directly but
 * workout_ctrl_tick() still re-issues the stale workout speed ~30 s later.
 *
 * For ACT_STOP, kmh is ignored; the keepalive will not re-assert anything.
 * For ACT_SPEED, kmh becomes the new keepalive value. */
void workout_ctrl_note_manual(int kind, float kmh);
