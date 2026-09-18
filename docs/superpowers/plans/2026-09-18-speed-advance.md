# Speed advance — command the next step's pace before the boundary

**Date:** 2026-09-18
**Status:** Plan, pre-implementation
**Default:** 5 s advance, configurable from the data field's Connect IQ settings
**Touches:** `watch/garmin_data_field`, `core/workout_ctrl.*`, `firmware/ble_ctrl_svc.c`,
`test/host`, `test/mock`, `test/pace_lag_report.py`, `docs/`

## Problem

Today the belt is told about a speed change *at* the workout step boundary
(`onWorkoutStepComplete` → frame → `workout_ctrl_on_frame` → `machine_set_speed`).
The command path is now fast (effective lag 1.371 s on the 2026-08-03 trace,
`docs/pace-lag-analysis.md`), but the belt itself still has to ramp: a
treadmill accelerating from a 4 km/h rest walk to a 14 km/h work pace spends
several seconds of every work interval below target, and the same on the way
down. That ramp is physical and no amount of BLE tuning removes it.

The fix is to move the command earlier: if the bridge knows what the *next*
step wants and how long the *current* step has left, it can command the next
speed `advance_s` seconds before the boundary so the belt is at pace when the
watch's step actually starts.

Two things stand in the way:

1. The 15-byte workout frame carries only the current step. The watch has the
   next one (`Activity.getNextWorkoutStep()`, API 3.2.0 — the project's
   `minApiLevel` already) but never sends it.
2. Nothing knows the remaining time in the current step. Connect IQ does not
   expose it; it has to be derived on the watch from `Activity.Info.timerTime`
   and the step's `durationValue`.

## Goals

- The data field has the next step (intensity, target, resolved pace) and the
  remaining time in the current step, and shows both.
- The bridge commands the next step's resolved speed `advance_s` before the
  boundary. Default 5 s. Settable 0–30 s from the data field's app settings;
  0 turns the feature off.
- The policy lives in `core/workout_ctrl.c` and is unit-tested on the host,
  mirrored in the mock bridge (through `libworkout_probe.so`, unchanged ABI)
  and in the pace scorer. The watch stays a telemetry packer.
- Old watch builds keep working against new firmware (v1 frames still accepted).

## Non-goals

- Advancing a **stop**. The end of the last step is the timer stopping, and
  that is still commanded on the timer transition.
- Advancing steps whose end the watch cannot predict (lap-button, HR, calorie
  durations). Those get no pre-roll and behave exactly as today.
- Predicting the treadmill's ramp rate. `advance_s` is a fixed lead, not a
  model of the belt.

## Design

### Where the decision is made

The **bridge** decides, not the watch. The watch reports raw facts (next step,
remaining seconds, the user's `advance_s` setting); `decode_action()` turns
them into a command. This keeps the "device is the brain" split, makes the
policy testable with `make host-test`, and lets the mock bridge and the pace
scorer model it without a second implementation.

The decision is a **pure function of the frame** — no bridge-side countdown,
no per-step state — so the Python mirror in `pace_lag_report.py` stays a
one-to-one transcription and the probe ABI does not change.

### Wire format v2 (20 bytes)

20 bytes is the ceiling: the firmware's `NRF_SDH_BLE_GATT_MAX_MTU_SIZE` is 23
and the watch does not negotiate, so a write payload is ATT MTU − 3. Bytes 0–9
are byte-for-byte v1. `durationValue` shrinks from u32 to u16 to make room; it
was informational only, and no step in a treadmill workout needs more than
65535 s or 65535 m.

```
[0]      version           = 2
[1]      timerState        (unchanged)
[2]      flags             bit0 FLAG_HAS_STEP     (unchanged)
                           bits1-4 diagnostics    (unchanged)
                           bit5 FLAG_HAS_NEXT     next step resolved, bytes 15-18 valid
                           bit6 FLAG_ADV_UP_ONLY  pre-roll only speed increases
[3]      intensity         (unchanged)
[4]      targetType        (unchanged)
[5..6]   targetLow  mm/s   (unchanged)
[7..8]   targetHigh mm/s   (unchanged)
[9]      durationType      (unchanged)
[10..11] durationValue u16 seconds for TIME, metres for DISTANCE, else raw; clamped
[12..13] remaining_s   u16 seconds left in the current step
                           0xFFFF = unknown, 0xFFFE = "far" (see send gating)
[14]     repetitionNumber  (unchanged)
[15]     nextIntensity     0xFF when no next step
[16]     nextTargetType    0xFF when no next step
[17..18] nextTarget mm/s   resolved midpoint of the next step's speed range; 0 = none
[19]     advance_s         0 = feature off; watch clamps to 0..30
```

The next step is sent pre-resolved to its midpoint rather than as low/high
because the byte budget does not stretch to two more u16s, and the midpoint is
the only thing the bridge ever does with a range anyway. The current step
keeps its raw low/high so nothing about v1 semantics moves.

`WORKOUT_FRAME_VERSION` becomes 2 and `WORKOUT_FRAME_LEN` 20;
`WORKOUT_FRAME_LEN_V1` (15) stays so `workout_ctrl_on_frame()` can accept
either. A v1 frame decodes exactly as today (no next, remaining unknown,
advance 0).

### Bridge policy (`core/workout_ctrl.c`)

Factor the existing target resolution out of `decode_action()` into

```c
/* SPEED target → midpoint; rest with no usable speed → REST_SPEED_KMH; else NONE */
static action_kind_t resolve_speed(uint8_t intensity, uint8_t target_type,
                                   uint16_t low_mmps, uint16_t high_mmps, float *kmh);
```

and use it for both slots. `decode_action()` becomes:

```
if timer != ON                                   → ACT_STOP           (unchanged)
cur = resolve_speed(current slot)
if version >= 2
   && advance_s > 0
   && FLAG_HAS_NEXT
   && remaining_s not in {0xFFFF, 0xFFFE}
   && remaining_s <= advance_s
   && !(durationType == TIME && durationValue < 2 * advance_s)      /* short-step guard */
   nxt = resolve_speed(next slot)
   if nxt == ACT_SPEED
      && !(FLAG_ADV_UP_ONLY && cur == ACT_SPEED && nxt.kmh < cur.kmh)
      → ACT_SPEED nxt.kmh
return cur                                       (ACT_SPEED or ACT_NONE, unchanged)
```

Notes on the rules:

- **Short-step guard.** A step shorter than twice the advance would be
  pre-empted almost as soon as it started. Such steps run at their own speed
  and the next step is commanded at the boundary, as today. The guard only
  knows TIME durations; distance steps rely on `remaining_s` alone.
- **Rest next.** A rest step with an OPEN target resolves to `REST_SPEED_KMH`
  through the same function the current slot uses, so the belt eases to the
  walk 5 s early. Symmetric by default: the aim is the belt's *actual* profile
  aligned with the watch's step timer, and the ramp down is as slow as the ramp
  up. `FLAG_ADV_UP_ONLY` is the opt-out for runners who want the full work
  interval on the belt and do not mind the rest starting late.
- **Non-speed next.** If the next step has no usable speed (active step with an
  HR/open target, or a free run), nothing is pre-rolled and the current
  command holds — same as what happens at the boundary today.
- **The boundary frame is a no-op.** When the step actually changes, the
  current slot now carries the speed that was already commanded; the existing
  `SPEED_EPS_KMH` dedup swallows it. That matters on iFit, where a repeated
  set re-triggers the console countdown.
- **Pause during pre-roll** → `ACT_STOP` from the timer-state rule, unchanged.
  **Keepalive** re-asserts whatever was last latched, unchanged.
- `advance_s` is clamped on the bridge too (`ADVANCE_MAX_S 30`) so a corrupt
  byte cannot pre-roll a whole step.

Lead accuracy is `advance_s ± 0.5 s`: the watch computes `remaining_s` at its
1 Hz `compute()` and the frame goes out when the integer crosses.

### Watch (`watch/garmin_data_field`)

**Next step.** `_packFrame()` also calls `Activity.getNextWorkoutStep()`,
applies the same resolution as the current slot (intensity off the
`WorkoutStepInfo`, drill through `.step`, speed midpoint in mm/s) and fills
bytes 15–18 plus `FLAG_HAS_NEXT`. `null` → 0xFF/0xFF/0, flag clear. The whole
block sits inside the existing try/catch so a throw degrades to "no next", not
to a lost frame.

**Remaining time.** New bookkeeping in `DataFieldView`:

```
mStepStartTimerMs   timerTime when the current step began (null = unknown)
mStepStartDistM     elapsedDistance when it began
mStepKey            the current-slot bytes [3..11] + [14] of the last packed frame
```

The step start is captured (a) in `onWorkoutStepComplete()`, (b) in
`onTimerStart()`, and (c) as a fallback in `compute()` when `mStepKey` changes
and no boundary was recorded in the last 2 s — because, as the existing
comment on `onWorkoutStepComplete` says, the step machine can lag the callback.
`timerTime` excludes paused time, so pauses need no special handling.

```
TIME:      remaining = durationValue_s − (timerTime − start) / 1000
DISTANCE:  remaining = (durationValue_m − (elapsedDistance − startDist)) / max(currentSpeed, 0.5)
other:     unknown (0xFFFF)
no start:  unknown (field added mid-step, or the watch never fired a boundary)
```

Rounded up to whole seconds and clamped to 0..0xFFFD.

**Send gating.** The frame is change-gated, and `remaining_s` changes every
second, which would turn a quiet timed step into a 1 Hz write stream. So the
watch only puts the exact value on the wire inside the window the bridge can
act on: `remaining_s <= advance_s + REMAIN_WINDOW_S (3)`; outside it the field
is 0xFFFE ("far"). Writes per step: one at the boundary plus roughly
`advance_s + 3` one-second frames at the end. The `CtrlBleDelegate` write
queue already handles back-to-back writes.

**Settings.** `resources/properties.xml` + `resources/settings.xml`:

| key | type | default | range |
|---|---|---|---|
| `advanceSec` | number | **5** | 0–30 |
| `advanceUpOnly` | boolean | false | |

Read in `initialize()` and again from `AppBase.onSettingsChanged()` so a change
from the Connect IQ phone app lands without restarting the activity. The
values go into byte 19 and flag bit 6 of every frame; a settings change
therefore re-sends by itself through the change gate.

**Display.** The middle row (currently just the timer state) becomes the next
step: `NEXT 12.0 in 0:45`, `NEXT walk in 0:12`, `NEXT 12.0` when remaining is
unknown, `LAST STEP` when there is no next, blank on a free run. The timer
state moves onto the top row (`8.5 km/h RUN`). The bottom row keeps
`CONN <stamp>` untouched — that stamp is how a sideload is verified and this
change will need it.

**Units to verify before trusting any of this.** The Connect IQ docs do not
say what unit `WorkoutStep.durationValue` uses for TIME (seconds or ms) or
DISTANCE (m or cm). Phase 0 below settles it with the mock bridge. Keep the
conversion in one named constant (`DUR_TIME_DIV`) so the answer is a one-line
change.

### Firmware (`firmware/ble_ctrl_svc.c`)

- `wkt_log()` accepts v1 and v2 (length ≥ 15 with v1, ≥ 20 with v2) and prints
  the next slot and advance on a third line. The change-detection key excludes
  `remaining_s` (bytes 12–13) so the end-of-step burst does not spam the RTT
  ring; instead log one `ctrl_svc: wkt ADV next=<kmh> rem=<s>` line the first
  time a step's frame satisfies `remaining_s <= advance_s`.
- `CTRL_WKT_MAX_LEN` (32) already fits. Nothing else in the firmware changes;
  the decision is in `core/`.

### Tooling

- **`test/mock/wkt_decode.py`** decodes v1 and v2; the log shows
  `next=12.0 km/h(rest) rem=4s adv=5s` and the probe's prediction now says
  `ACT_SPEED 12.0 km/h [pre-roll]` when it fired early. The probe ABI is
  unchanged: `probe_feed()` already reports what `workout_ctrl.c` commanded.
- **`test/pace_lag_report.py`** gains `--advance-s` (default **0**, a property
  of the recording like `--sdm-cycle-s`) and `--advance-up-only`. The
  command-signal builder shifts each lap boundary earlier by `advance_s` when
  the previous lap was a TIME step at least `2 × advance_s` long and the new
  target resolves to a speed (and, with up-only, is faster). Makefile:
  `ADVANCE ?=` → `--advance-s $(ADVANCE)`. The existing baselines were
  recorded without pre-roll and keep scoring with the default.
- **`test/check_uuid_contract.py`** (or a sibling script run from
  `check-uuid`) additionally asserts `FRAME_VERSION`/`FRAME_LEN` agree between
  `workout_ctrl.h`, `DataFieldView.mc` and `wkt_decode.py`. Cheap, and this
  change is exactly the kind of three-way edit that drifts.

### Compatibility and rollout order

| watch build | firmware | result |
|---|---|---|
| v1 | new | works as today (no pre-roll) |
| v2 | new | pre-roll |
| v2 | old | **belt does nothing** — old firmware drops v2 as `MALFORMED` |

The watch cannot detect firmware age (the data field is write-only), so the
order is: flash the firmware first, then sideload the data field, and read the
`CONN <stamp>` row before judging anything.

## Implementation phases

Each phase leaves `make host-test` green and is a separate commit.

### Phase 0 — establish the facts (mock bridge, no firmware)

1. Add a temporary `System.println` of `durationType`, `durationValue`,
   `getNextWorkoutStep()` (intensity, target, low/high, duration) and
   `timerTime` at each `onWorkoutStepComplete()`, run a workout with a known
   60 s TIME step, a 400 m DISTANCE step, and a 3-rep interval (work + rest) in
   the simulator and on the watch against `make mock-bridge`.
2. Record: the unit of `durationValue`; whether `getNextWorkoutStep()` inside
   an interval returns the rest portion (or the next repetition's work
   portion); whether the callback fires before or after the step machine
   advances. Write it into `watch/README.md`.

Exit: the three questions above have answers. If interval steps do not expose
the rest portion as "next", the rest-slot fallback is: next = rest at
`REST_SPEED_KMH` whenever the current step is the work portion of an interval
(`repetitionNumber != 0` and intensity active). Decide then, not before.

### Phase 1 — core policy + host tests

Files: `core/workout_ctrl.h`, `core/workout_ctrl.c`, `test/host/test_workout_ctrl.c`.

- Version/length constants, `resolve_speed()` refactor, v2 decode, the rule
  block above.
- Tests (build v2 frames with a new `frame2()` helper next to `frame_i()`):
  - v1 frames: every existing test unchanged and still passing.
  - v2 with no next / advance 0 / remaining unknown / remaining far → identical
    to v1 behaviour.
  - remaining == advance → next speed commanded; remaining == advance + 1 → not.
  - boundary frame after a pre-roll → no second `machine_set_speed` (dedup).
  - next = rest with OPEN target → `REST_SPEED_KMH` early.
  - next = active with HR target → current holds.
  - short-step guard: TIME step of `2·advance − 1` s → no pre-roll; `2·advance` → yes.
  - up-only flag: slower next not pre-rolled, faster next is.
  - pause mid-pre-roll → stop; resume → the latched (pre-rolled) speed comes back.
  - `advance_s = 200` in the frame → clamped, behaves as 30.
- `grep` purity check on `core/` still empty.

### Phase 2 — mock + scorer

Files: `test/mock/wkt_decode.py`, `test_wkt_decode.py`, `mock_bridge.py`,
`test/pace_lag_report.py`, `Makefile`, `test/check_uuid_contract.py`.

- v2 decode + tests; mock log lines; `[pre-roll]` annotation.
- Scorer `--advance-s` / `--advance-up-only`; a `--self-test` case with a
  synthetic run whose transitions lead by 5 s must score ~0 lag with
  `--advance-s 5` and ~+5 s without.
- Frame-contract check wired into `check-uuid`.
- `make pace-test` on the archived traces still passes with no args.

### Phase 3 — watch

Files: `DataFieldView.mc`, `DataFieldApp.mc`, `resources/properties.xml`,
`resources/settings.xml`, `watch/README.md`.

- Next-slot packing, remaining-time bookkeeping, send window, settings,
  display rows.
- Validate against `make mock-bridge` with the Phase 0 workout: the log must
  show `far` through the middle of each step, a countdown at the end, the
  probe firing `[pre-roll]` 5 s before the boundary, and the boundary frame
  itself decoding to `ACT_NONE` (deduped).
- `make ciq-build` (stamp first), then `make sideload`; confirm the stamp on
  the watch.

### Phase 4 — firmware

Files: `firmware/ble_ctrl_svc.c` (logging only), `CLAUDE.md` contract line
("raw 15-byte" → "15-byte v1 or 20-byte v2"), `docs/`.

- Build with `make firmware`, `make dfu`, push over USB (`DFU` on the console,
  `make flash-dfu`).

### Phase 5 — hardware gate

1. Firmware first, then the data field; read the stamp.
2. Do not reuse the standard `10 s / 10 s / 10 s rest` interval workout: with
   a 5 s advance every 10 s step sits exactly on the `2 × advance` edge of the
   short-step guard. Use `30 s @ 10.8 / 30 s @ 11.4 / 30 s rest`, 10 rounds,
   so pre-roll is unambiguous. Run it once in SDM:TGT mode and once in
   actual-belt mode.
3. SDM:TGT trace → `make pace-test FIT=… ADVANCE=5 BASELINE=<new>`: with the
   advance modelled, `effective_lag_s` should land near the 1.37 s of the
   post-fix trace, and `--advance-s 0` on the same file should read roughly
   `1.37 − 5 ≈ −3.6 s` edge lag (commands leading the laps). Store the new
   baseline as `test/baselines/<id>-advance-5.json`.
4. Actual-belt trace: the acceptance criterion. Measure, per work interval,
   the time from the lap boundary to the belt reaching within 0.2 km/h of
   target. Expect it to drop from "several seconds" to ≲1 s. Record the
   numbers in `docs/pace-lag-analysis.md` §"Speed advance".
5. Confirm the middle row on the watch counts down and matches the belt.

## Risks and open questions

- **`getNextWorkoutStep()` inside intervals.** If it returns the next
  repetition's work step instead of the rest portion, the rest pre-roll needs
  the fallback in Phase 0. Decided by measurement.
- **`durationValue` units.** Unknown from the docs; Phase 0.
- **Boundary detection.** `onWorkoutStepComplete` may fire before the step
  machine advances; the frame-diff fallback covers it, and a wrong start only
  makes `remaining_s` wrong by the callback-to-advance gap (≤1 s observed).
- **Recording semantics change.** In SDM:TGT mode the footpod now broadcasts
  the pre-rolled target, so the last 5 s of each lap in Garmin Connect show
  the *next* pace. That is what the belt is doing, so it is truthful, but the
  scorer must be told (`ADVANCE=5`) or it reports negative lag.
- **Distance steps** depend on `currentSpeed`, which on a treadmill is the
  bridge's own SDM broadcast. Good enough for a ±1 s lead; not for anything
  finer.
- **BLE write burst.** ~8 writes at 1 Hz per step end. Write-with-response at
  a 30–60 ms interval; the queue keeps only the newest frame, so a slow link
  degrades to a later pre-roll, never to a wrong one.
- **Free run, unchanged.** No step → no next → nothing pre-rolled; the belt is
  not touched, exactly per the existing "a free run does not move the belt"
  rule in `CLAUDE.md`.

## Definition of done

- `make host-test` green, including the new frame-contract check.
- `make pace-test` unchanged on the archived traces; new advance-5 baseline
  stored and passing.
- Mock-bridge session log in `docs/superpowers/test-logs/` showing the
  countdown and the `[pre-roll]` prediction.
- Hardware run: actual-belt trace with the interval-to-target time recorded,
  stamp confirmed, results added to `docs/pace-lag-analysis.md`.
- `CLAUDE.md` control contract and `watch/README.md` wire-format sections
  updated to v2.

---

## Implementation notes (Phases 1–4 landed; 0 and 5 outstanding)

Phases 1–4 are implemented. **Phase 0 and Phase 5 are not**: neither the
Connect IQ simulator/SDK nor the nRF5 toolchain nor the hardware was available,
so the watch (`.mc`) and firmware (`ble_ctrl_svc.c`) changes are **written but
never compiled**, and no measurement exists.

Phase 0's two open questions were therefore taken as *stated assumptions*,
each a one-line change to flip, and both are written up in `watch/README.md`:

- `WorkoutStep.durationValue` is assumed **seconds** for TIME and **metres**
  for DISTANCE, behind `DUR_TIME_DIV` / `DUR_DIST_DIV` in `DataFieldView.mc`.
- `Activity.getNextWorkoutStep()` is assumed to return the next *portion* to
  run — the rest portion while the work portion is running. If it returns the
  next repetition's work step instead, the rest never pre-rolls and this plan's
  Phase 0 fallback is needed.

Where the implementation departs from the plan above, and why:

- **Frame-contract check.** The watch's `(FRAME_VERSION, FRAME_LEN)` is checked
  for membership in the set of versions the bridge accepts, not for equality
  with the newest. A v1 watch build driving new firmware is a supported
  configuration — it is the whole reason v1 is still accepted — so requiring
  equality would fail the gate on a configuration the compatibility table above
  calls "works as today". `workout_ctrl.h` and `wkt_decode.py` are still
  compared for exact equality: they are two sides of one decision.
- **`durationType`/`durationValue` moved ahead of the target-type gate in
  `_packFrame`.** v1 packed them only on speed steps, so a rest step — which
  arrives with an OPEN target — carried no duration at all and could never
  produce a `remaining_s`. Easing down into the rest is half the point of the
  advance, so the duration is now packed for every step shape.
- **`_packNext()` has its own try/catch**, nested rather than sharing the
  current step's. Same degradation ("no next step"), but a throw while reading
  the next step cannot cost the current step's target, which is the main
  product path.
- **The display has a sixth case**, `NEXT --`, for a next step that exists but
  resolves to no usable speed (an active step with an HR or open target). The
  bridge pre-rolls nothing there and the belt holds, which is worth showing
  rather than rendering as a blank row.
- **The pace scorer measures "at least 2 × advance long" from the lap**, not
  from the step's nominal `duration_value`: the lap is what actually ran, and
  it avoids depending on the FIT field's units. The `duration_type == "time"`
  gate is still the step's.
- **`ADVANCE_UP_ONLY=1`** was added to the Makefile beside `ADVANCE=`, since a
  trace recorded with `FLAG_ADV_UP_ONLY` set needs both to score correctly.

Still to do, in order: Phase 0 on the simulator/watch against
`make mock-bridge` (confirm the two assumptions, and that the log shows `far`
mid-step, a countdown at the end, and a `[pre-roll]` prediction), `make
ciq-build` + `make firmware` to prove both sides compile, then Phase 5.
