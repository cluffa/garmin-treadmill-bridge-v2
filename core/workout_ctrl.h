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
 *   - scale the whole workout's difficulty from a single speed adjustment
 *     the user makes on any one interval (see below).
 *
 * ---- Difficulty scaling ----------------------------------------------------
 *
 * Every workout-derived target is multiplied by a scale factor before being
 * commanded:  commanded = base * scale.  `base` is the *unscaled* target the
 * step decodes to, which is what makes repeated adjustments compound: drop a
 * 6 km/h interval to 5.4 and scale becomes 0.90, so the next 7 km/h interval
 * runs at 6.3; drop that to 5.67 and scale becomes 5.67/7.0 = 0.81.
 *
 * The scale is learned from the adjustment the user already makes — there is
 * no new input on the board or the watch:
 *   - the treadmill's own speed buttons, seen as the belt diverging from what
 *     we commanded (fed in by workout_ctrl_note_belt), or
 *   - an explicit SPEED from the ctrl app (workout_ctrl_note_manual).
 * Either way scale = observed / base. A ratio outside [0.5, 1.5] is discarded
 * rather than clamped — a pulled safety key reads as ~0 km/h, and clamping
 * that to 0.5 would restart the next interval at half pace unasked.
 *
 * Scale resets to 1.0 on TIMER_STATE_OFF (a fresh, not-yet-started activity)
 * and at boot. It deliberately does NOT reset in workout_ctrl_reset(): that
 * fires on watch disconnect, and losing the user's adjustment to a transient
 * BLE drop mid-run is worse than any staleness it would avoid.
 *
 * Platform-agnostic: depends only on machine.h (no SoftDevice / nRF / ESP
 * includes), so both the nRF52840 and ESP32 control services share it. Each
 * board hands raw writes to workout_ctrl_on_frame() and calls
 * workout_ctrl_tick() at ~1 Hz from an existing timer.
 *
 * ---- Wire format (little-endian, WORKOUT_FRAME_LEN bytes) ------------------
 *   [0]     version           (WORKOUT_FRAME_VERSION)
 *   [1]     timerState        (Activity.TIMER_STATE_*: 0 off,1 stopped,2 paused,3 on)
 *   [2]     flags             (bit0 = a workout step is present; bits 1-4 are
 *                              watch-side diagnostics the bridge ignores:
 *                              0x02 pack threw, 0x04 no step resolved,
 *                              0x08 step resolved but targetType missing,
 *                              0x10 throw was in the duration-value region)
 *   [3]     intensity         (Activity.WORKOUT_INTENSITY_*: 0 active,1 rest,…;
 *                              0xFF when no step resolved. The bridge acts on
 *                              this: a rest step with no usable speed target
 *                              is commanded to REST_SPEED_KMH rather than
 *                              holding the work-interval speed.)
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
 * re-asserting a stale target. Does NOT reset the difficulty scale — see the
 * header comment. */
void workout_ctrl_reset(void);

/* Feed the actual belt speed, in km/h, at roughly 1 Hz while a treadmill is
 * connected. This is how a speed change made on the treadmill's own console
 * becomes a difficulty adjustment: once the belt has been seen to *reach* the
 * commanded speed, a sustained divergence from it is adopted as the new scale.
 *
 * Arming on reach rather than on a settling timer is deliberate. A fixed
 * post-command blackout cannot cover both a 0.2 km/h nudge and a 4->12 km/h
 * ramp, and arming on reach also means a treadmill whose ceiling is below the
 * authored target never arms — hardware limits are never mistaken for a user
 * adjustment.
 *
 * Safe to call when nothing is latched; it is a no-op then. Callers should
 * skip it while the treadmill link is down rather than feeding a stale
 * reading. */
void workout_ctrl_note_belt(float kmh);

/* Current difficulty scale: 1.0 = run the workout exactly as authored, 0.9 =
 * every target 10% slower. Always within [0.5, 1.5]. */
float workout_ctrl_scale(void);

/* The unscaled target of the latched workout step, in km/h; 0.0 when no step
 * target is latched (free run, or nothing received yet). Reported for
 * display/telemetry — the commanded speed is this times the scale. */
float workout_ctrl_base_kmh(void);

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
