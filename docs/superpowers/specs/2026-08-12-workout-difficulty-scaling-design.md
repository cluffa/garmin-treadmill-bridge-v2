# Workout difficulty scaling — design

Date: 2026-08-12
Status: accepted (implemented on branch `workout-difficulty-scaling`)

## Problem

A structured workout's speed targets are fixed when the workout is authored. On
the day, they are often wrong — the whole session is a notch too hard (or too
easy). Today the only recourse is to override the belt speed on every single
interval, because the bridge re-asserts the authored target on the next step
change and on the ~30 s keepalive.

Wanted: change the speed **once**, on any interval, and have every subsequent
interval carry that same relative change. Cut 6 km/h by 10% and the next
7 km/h interval runs at 6.3. No new button, no new watch setting, no new
ctrl-grammar verb — the adjustment the user already makes *is* the input.

## Model

One number: **`scale`**, a dimensionless multiplier, default 1.0.

```
commanded_kmh = clamp(base_kmh * scale)
```

`base_kmh` is the *unscaled* target the workout step decodes to (the
low/high midpoint, or `REST_SPEED_KMH` for a rest step). Storing the base
rather than the commanded value is what makes repeated adjustments compound
correctly for free:

| step | base | scale | commanded | user sets | new scale     |
|------|------|-------|-----------|-----------|---------------|
| 1    | 6.0  | 1.00  | 6.0       | 5.4       | 5.4/6.0 = 0.90 |
| 2    | 7.0  | 0.90  | 6.3       | —         | 0.90           |
| 3    | 7.0  | 0.90  | 6.3       | 5.67      | 5.67/7.0 = 0.81 |

Scale is learned as `observed / base`, never as an increment, so it is always
consistent with what the belt is actually doing.

## How scale is learned (two paths, no new inputs)

**1. Belt divergence (the main path).** The user reaches down and presses the
treadmill's own speed-down button. The bridge already receives actual belt
speed from FTMS/iFit telemetry. A new `workout_ctrl_note_belt(kmh)` is called
at ~1 Hz from `heartbeat_cb()` in `main.c`, guarded by `machine_connected()`.

The detector is **armed on reach, not on a timer**: it starts watching only
after the observed speed has *reached* the commanded speed once (inside the
deadband). A fixed post-command blackout would get the ramp wrong in both
directions — an iFit 4→12 ramp outruns any constant — and arm-on-reach also
means a treadmill whose ceiling is below the authored target (belt never
reaches 14 km/h) never arms, so hardware limits are never mistaken for a user
adjustment.

Once armed, a divergence must exceed **both** a relative (4%) and an absolute
(0.2 km/h) deadband, for **3 consecutive samples** (~3 s), before it is
adopted. Requiring both bands keeps iFit's coarse speed quantization at a
4 km/h rest step from tripping a pure-relative threshold.

**2. Explicit `SPEED` from the ctrl app.** `workout_ctrl_note_manual(SPEED,
kmh)` already latches manual commands. It now also learns `kmh / base` when a
workout base is latched. It is deterministic, so it learns immediately with no
arming or confirmation. The commanded value is the user's absolute request,
*not* re-scaled. `note_manual(STOP)` never learns.

### Out-of-range ratios are discarded, never clamped

If `observed / base` falls outside **[0.5, 1.5]** the sample is **dropped
entirely** — the scale is left alone. This is the one genuinely dangerous
failure mode: a pulled safety key or a console STOP reads as observed ≈ 0, and
clamping that to 0.5 would restart the *next* interval at half pace with no
one having asked for it. It has its own named test.

### Keepalive interaction

`workout_ctrl_tick()` defers the ~30 s keepalive by one cycle while a
divergence is mid-confirmation (diverging, not yet adopted). Without this, a
console change landing just before a keepalive tick gets stomped back to the
old target one second before the bridge would have accepted it. Once a
divergence is adopted, the latched speed becomes the observed speed, so the
keepalive re-asserts the user's value and the system sits at a fixed point.

## Reset policy

Scale resets to 1.0 **only** on `TIMER_STATE_OFF` (activity not started) and at
boot. That gives every fresh activity a clean 1.0 without carrying yesterday's
adjustment.

It deliberately does **not** reset in `workout_ctrl_reset()`. That is called on
watch disconnect, and a CIQ BLE wedge or a transient drop mid-run is a real
occurrence on this hardware — losing the user's difficulty adjustment because
the link blinked would be worse than any staleness it avoids. The asymmetry is
documented in `workout_ctrl.h`.

Pause and stop (`PAUSED` / `STOPPED`) preserve scale: a mid-run break must not
undo the adjustment.

## Display

Testboard row 6 (previously spare) renders:

```
Diff: 90% Base:   6.0
```

exactly 21 columns. `Diff` is the scale as an integer percent; `Base` is the
current unscaled step target, so the row shows the input and the multiplier and
row 2 (`Targ:`) already shows the product.

`STATUS` gains `"scale":90` (integer percent). That is the only observability
path with `TESTBOARD=0`, and it costs nothing.

## Scope

`core/workout_ctrl.c` owns the scale — not a new module. It already owns the
latch, the base target and the keepalive, all of which the scale has to
interact with; splitting it out would mean exporting that state.

`core/` purity is preserved: the belt feed enters through one new
platform-agnostic function called from `main.c`. The two `hvx` telemetry paths
in `ble_central.c` are untouched.

## Non-goals

- **Incline is not scaled.** The request is speed only.
- No persistence across reboot. Scale lives in RAM.
- No watch-side UI, no new ctrl verb, no config setting — by requirement.
- The bridge does not scale a *manual* `SPEED` command's own value. A manual
  command is an absolute request and is obeyed literally.

## Test plan

Host tests in `test/host/test_workout_ctrl.c`:

- a `SPEED` override learns the ratio, and the next step's target is scaled
- two successive overrides compound (0.9 → 0.81)
- belt divergence arms only after the belt reaches the commanded speed
- a sustained divergence is adopted after 3 samples, not before
- a belt reading of ~0 (safety key) is **discarded**, scale unchanged
- `STOP` never learns
- `TIMER_STATE_OFF` resets scale; `PAUSED`/`STOPPED` and
  `workout_ctrl_reset()` do not
- the keepalive defers while a divergence is mid-confirmation
- rest steps are scaled too (a scaled session eases the walk breaks as well)

`make pace-test` must still pass unchanged: scale defaults to 1.0, so the
recorded baseline is unaffected. The `REST_SPEED_KMH` `#define` line stays
textually intact because `test/pace_lag_report.py` regex-parses it.

## Hardware verification

On the desk (no treadmill): flash, confirm the new build stamp via `STATUS`
before trusting anything, confirm `STATUS` reports `scale`, confirm the testboard
`Diff:` row renders, and drive INJECT + a console `SPEED` write to show
learn-and-apply end to end.

Belt-divergence learning from a real console adjustment needs a treadmill and
is left as the one residual gap.
