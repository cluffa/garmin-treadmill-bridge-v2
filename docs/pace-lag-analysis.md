# Pace lag analysis — 2026-08-01

How fast does the belt actually follow the watch's workout targets, where does
the delay come from, and what was changed to reduce it.

Source data: `test/23806153959_ACTIVITY.fit` — 10 rounds of
`10 s @ 10.76 km/h / 10 s @ 11.36 km/h / 10 s rest`, run with the bridge in
**SDM:TGT** mode (the footpod broadcasts the bridge's resolved target instead of
the belt's actual speed), on the firmware at `4932511` minus the rest-step
change — i.e. rest steps still held the work pace.

Tooling: [`test/pace_lag_report.py`](../test/pace_lag_report.py), `make pace-test`.

---

## 1. Method

The .FIT carries everything needed to grade itself:

| what | from |
|---|---|
| the target the bridge should have resolved | `workout_step_mesgs` (mm/s low/high, intensity, target type) |
| when each step actually started | `lap_mesgs` (`start_time`, `wkt_step_index`, `intensity`) |
| what the bridge actually commanded | `record_mesgs.enhanced_speed` — in SDM:TGT mode this *is* the commanded target, round-tripped through ANT |
| whether the bridge kept running through a glitch | `record_mesgs.distance` — integrated from the same target |

The script rebuilds the reference command `c(t)` by running the workout steps
through a Python mirror of `core/workout_ctrl.c decode_action()` (same integer
mm/s midpoint, same `mm/s → km/h → m/s` float32 chain, same `ACT_NONE` latching
semantics), then scores the recorded trace `a(t)` against it.

Both signals are piecewise constant, so every integral is an exact finite sum,
not a quadrature approximation.

### The headline number

For a pure transport delay `τ` acting on a speed step of size `|Δ|`, the area
between command and response is exactly `|Δ|·τ`. So

```
effective_lag = ∫|a(t) − c(t)| dt  /  Σ|Δᵢ|
```

is a **lag in seconds** that also absorbs dropouts and level errors — one
number that goes down when and only when the belt tracks better. It is reported
next to two independent estimators that must agree with it on a clean trace:
the per-transition edge lag, and the global shift `τ*` that minimises the area.

The area is split into **transient** (inside the response window after a
commanded change), **dropout**, and **steady** (level error) buckets that sum
back to the total, so a regression can be attributed instead of just observed.

The scorer is checked against synthetic signals with known answers before it is
trusted to judge firmware: `./test/pace_lag_report.py --self-test`.

---

## 2. Results on the 2026-08-01 trace

```
effective lag              4.393 s      <- IAE / Σ|step|, all error folded in
  of which transient       2.234 s      <- the actual response delay
best global shift τ*       2.000 s
edge lag        mean 2.26 s  median 2.0  p90 3.0  min 0.0  max 5.0
IAE total                  13.94 m      (1.50% of the 930 m commanded)
  transient                 7.09 m
  dropouts                  6.31 m
  steady state              0.54 m
steady-state gain          0.99926  (−0.074%)
```

**Every one of the 19 commanded speed changes arrived** — nothing was lost, and
the belt never chased a stale target. The problem is purely how long each change
took, plus three telemetry glitches.

### 2.1 The lag is ~2.3 s, and it is not drift

The first two transitions land at 0 s lag and the next few at 1, 2, 3 — which
looks like an accumulating drift. It isn't: over the full 10 rounds the lag
oscillates in 1–3 s with a mean of 2.26 s and no trend. The apparent ramp at the
start is the phase between the watch's 1 Hz compute and the step boundary
walking through its period.

### 2.2 Only part of that lag reaches the belt

This matters for deciding what to fix. The measured 2.26 s is the round trip:

| term | s | reaches the belt? |
|---|---|---|
| watch's 1 Hz `compute()` — the step boundary lands at a uniformly random point in the period | 0–1.0 (≈0.5 avg) | **yes** |
| BLE write to `A6ED0004` (30–60 ms conn interval, write-with-response) | ~0.1 | **yes** |
| a write colliding with one already in flight, deferred to the next `compute()` | 0 or 1.0 | **yes** |
| bridge decode + `machine_set_speed` | negligible | yes |
| ANT broadcast staging + 4 Hz slot | 0–0.5 | no — measurement only |
| watch records one speed sample per second | 0–1.0 (≈0.5 avg) | no — measurement only |

Roughly **1 s of the 2.26 s is measurement artefact** that a real treadmill
never experiences. The reducible, belt-facing part is the watch-side 1 Hz
quantisation and the write-collision penalty — which is exactly what the fixes
below target. The ANT staging path was deliberately left alone: it costs real
firmware risk on the TX path to improve a number the belt never sees.

### 2.3 The speed dropouts are a page-schedule artefact, not RF loss

Five records had no usable speed while the bridge was commanding one
(`t=104,206,207,291,292` — a missing field, sometimes followed by a hard 0.0).
A 0 km/h spike in the middle of an interval is visible in Garmin Connect and
poisons any pace analysis.

Two independent pieces of evidence say the belt never slowed:

1. **Distance kept advancing straight through, at the commanded rate.** The SDM
   distance field is integrated from the same target the speed field carries,
   so if the bridge had dropped the target, distance would have flattened. It
   didn't — `distance kept 100%`. The script reports this automatically and
   labels the event *"telemetry glitch (distance kept advancing)"*.

2. **All five holes fall at the same phase of the ANT page cycle.** The channel
   period is `8192/32768 = 0.25 s` exactly and the rotation was 68 slots, so the
   cycle is exactly 17.000 s. The holes sit at phases `[2, 2, 3, 2, 3]` s —
   circular concentration **0.984** out of 1.0. Scoring the same trace against a
   deliberately wrong 16 s period drops the concentration to 0.263, which rules
   out the clustering being an artefact of the test.

The cause is the old page rotation:

```
slots 0..63  page 1 (speed + distance)
slots 64,65  page 2
slot  66     common page 80
slot  67     common page 81
```

Four background slots **back to back** at 4 Hz is a full second in which the
watch receives no page 1 at all — and the watch records one speed sample per
second. Every 17 s there was a window where a record could land with no fresh
speed to write down.

Page 2 made it worse rather than covering the gap: its status byte was `0x00`,
and bits [1:0] of that byte are the SDM **use state**, where 0 means *inactive*
(`ANT_SDM_USE_STATE_ACTIVE = 0x01` in the SDK's own
`ant_sdm_page_2.h`). The bridge was telling the watch "this footpod is not in
use" twice per cycle, which is the obvious source of the hard `0.0` samples
that follow the missing ones.

---

## 3. Changes made

| # | file | change |
|---|---|---|
| 1 | `watch/…/DataFieldView.mc` | send the frame from `onWorkoutStepComplete()` |
| 2 | `watch/…/CtrlBleDelegate.mc` | queue a frame that arrives mid-write and flush it on completion; invalidate the caller's "already sent" latch across a reconnect |
| 3 | `core/ant_sdm_encode.c` | page 2 status byte `0x00` → `0x01` (use state = active) |
| 4 | `firmware/ant_sdm.c` | spread the four background slots through a 64-slot cycle instead of bunching them |

**1 — step-boundary push.** `onWorkoutStepComplete()` is a `DataField` callback
(CIQ ≥ 3.0.0; the manifest requires 3.2.0) that fires *at* the step boundary
rather than at the next 1 Hz `compute()`. This removes the largest belt-facing
term, worth ~0.5 s on average and up to 1 s. It is deliberately change-gated
rather than a forced push: if the workout step machine hasn't advanced yet when
it fires, `_packFrame()` still returns the old step and nothing is sent, so the
existing 1 Hz path takes over. It can only help, never hurt.

**2 — no more waiting a second for the bus.** A frame arriving while a write was
in flight used to be dropped, with the next attempt a full `compute()` later.
That is the 1 s penalty visible in the 3 s and 5 s tail of the lag distribution.
It is now queued (newest wins — an older frame is by definition superseded) and
issued the instant `onCharacteristicWrite` fires. The same change fixes a
pre-existing gap: after a reconnect the view believed the bridge still held the
last frame it sent and wouldn't re-send until the target next changed.

**3 and 4 — no more 0 km/h spikes.** Page 2 now reports use state *active*, and
the background pages are spread (slots 15, 31, 47, 63 of a 64-slot cycle) so no
one-second window can be starved of page 1. The 64-slot cycle keeps each common
page inside the 65-message requirement of the ANT+ profile.

Not changed: the ANT TX staging path (§2.2 — measurement-only), and the belt
control policy in `core/workout_ctrl.c` (correct; every transition arrived).

---

## 4. Re-running it

```sh
make pace-test                                   # scorer self-check + baseline gate
make pace-report FIT=test/my-new-run.fit         # full report on a new run
./test/pace_lag_report.py FILE.fit -v            # + per-transition table
```

The gate compares against `test/baselines/23806153959-pre-fix.json` and exits
non-zero if any metric regresses beyond its tolerance. To grade the fixes, run
the **same** workout on the new firmware and:

```sh
./test/pace_lag_report.py test/NEW.fit --baseline test/baselines/23806153959-pre-fix.json
```

Baseline and candidate must come from the same workout — this compares firmware
revisions, not workouts.

### What success looks like

| metric | before | expected after | **actual (2026-08-03)** | |
|---|---|---|---|---|
| `edge_lag_mean_s` | 2.26 | 1.3 – 1.8 | **1.41** | ✅ |
| `edge_lag_max_s` | 5.0 | ≤ 3.0 | **3.0** | ✅ |
| `speed_holes` | 5 | 0 | **4** | ⚠ see below |
| `dropout_seconds` | 4.0 | 0 | **2.0** | ⚠ see below |
| `effective_lag_s` | 4.39 | ~1.5 | **1.371** | ✅ |
| `steady_gain` | 0.99926 | unchanged | 0.99717 | ✅ (-0.03 km/h @ 11 km/h) |

Measured on `test/23842067586_ACTIVITY.fit`, baseline
`test/baselines/23842067586-post-fix.json`.

The two ⚠ rows are **passes, not misses.** The prediction of zero was wrong
about the mechanism, not the fix: the page-schedule artefact is gone (hole
clustering 0.984 → 0.50 @16 s / 0.38 @17 s — unclustered at *either* candidate
cycle), and the page-2 use-state fix eliminated the false *zero* samples
entirely (2 → 0; every remaining hole is `missing`, never `zero`). That is why
`dropout` IAE went 6.31 m → **0.00 m** even though 4 holes remain — they no
longer corrupt the trace. The residual is ordinary RF loss, which nothing in
this firmware can remove.

An edge lag much below ~1.0 s is not achievable from a data field: the watch
still only records one speed sample per second, so ~0.5 s of the remaining
number is the measurement, not the belt.

### Two caveats when scoring the archived trace

* The hole-periodicity check needs the page-cycle length of the firmware that
  *recorded* the trace. Since 2026-08-03 `make pace-test` / `pace-report`
  default to the **post-fix** trace with `SDM_CYCLE` empty, which reads
  `CYCLE_LEN x SDM_CHANNEL_PERIOD` from `ant_sdm.c` (16.00 s today) and stays
  correct as that file changes. Scoring the **archived pre-fix** trace needs its
  own recording firmware's value passed back explicitly:
  `make pace-test FIT=test/23806153959_ACTIVITY.fit BASELINE=test/baselines/23806153959-pre-fix.json SDM_CYCLE=17.0`.
  The report always prints which value it used and where it came from.
* `--rest-policy` defaults to `auto` and correctly detects `hold` here (the
  `walk` model fits 14.8× worse). New traces from current firmware will detect
  `walk`. A baseline and a candidate recorded under different rest policies are
  not comparable, and the script says so.

---

## 5. Status

All four changes are written, and everything that can be verified without the
hardware has been:

* `make host-test` — passes, including a new assertion on the page 2 status byte
* `make firmware` — builds
* `monkeyc` build of `watch/garmin_data_field` — `BUILD SUCCESSFUL`
* `./test/pace_lag_report.py --self-test` — passes

**Verified on hardware 2026-08-03.** All four changes confirmed by
`test/23842067586_ACTIVITY.fit`; the measured column is in §4. Headline:
effective lag 4.393 s → **1.371 s**, dropout area 6.31 m → **0.00 m**, hole
clustering 0.984 → unclustered. Rest steps were confirmed at 4 km/h in the same
run (the scorer auto-detects `walk`, with `hold` fitting 3.9× worse).

⚠ **Do not compare `iae_m` across the two traces** — they are different
workouts. Pre-fix rested at work pace (Σ|Δ| ≈ 3.2 m/s over 19 changes); post-fix
rests at 4 km/h (Σ|Δ| ≈ 39 m/s over 29 changes), so absolute area is ~12× larger
by construction. `effective_lag_s` = IAE/Σ|Δ| normalises exactly this out and is
the only cross-workout comparable metric.

---

## 6. Speed advance — commanding the next step early

The 1.371 s above is the *command* path: the time from the watch's step
boundary to the bridge issuing the new target. What it does not cover is the
belt. A treadmill accelerating from a 4 km/h rest walk to a 14 km/h work pace
spends several seconds of every work interval below target, and the same
coming down. That ramp is physical, and no amount of BLE tuning removes it.

The fix is to move the command earlier: the data field now sends the *next*
step and how long the current one has left (wire format v2), and
`core/workout_ctrl.c` commands the next speed `advance_s` seconds before the
boundary, so the belt has finished ramping when the watch's step starts.
Default 5 s, settable 0–30 from the data field's Connect IQ settings, 0 = off.
The design, the rules and the guards are in
`docs/superpowers/plans/2026-09-18-speed-advance.md`; the policy itself is unit
tested by `make host-test`.

### What this means for this scorer

**A trace recorded with the advance on must be scored with `ADVANCE=` set to
the value the recording firmware was running.** It is a property of the trace,
exactly like `SDM_CYCLE`, not a tuning knob:

```sh
make pace-test FIT=test/<new>.fit BASELINE=test/baselines/<new>-advance-5.json ADVANCE=5
make pace-report FIT=test/<new>.fit ADVANCE=5   # add ADVANCE_UP_ONLY=1 if set
```

With the advance modelled, `command_signal()` moves each qualifying lap
boundary earlier by `advance_s`, so the reference command leads the laps the
same way the bridge did. Score the same file with `ADVANCE=0` and the scorer
sees the belt changing speed *before* it was told to, which it can only report
as several seconds of area between the curves — the pre-roll shows up as lag
rather than as the improvement it is. The scorer's `--self-test` pins both
halves of that on a synthetic run that leads by 5 s.

The archived baselines (`23806153959-pre-fix`, `23842067586-post-fix`) predate
the feature and keep scoring with the default empty `ADVANCE`. Baselines
written from here on record the advance they were scored with, and `compare()`
says so when a candidate disagrees.

### Numbers

**None yet — the hardware run is pending.** Phase 5 of the plan is the gate:
firmware first, then the data field, stamp confirmed, then `30 s @ 10.8 /
30 s @ 11.4 / 30 s rest` × 10 (deliberately not the usual 10 s steps, which sit
exactly on the short-step guard's `2 × advance` edge), run once in SDM:TGT mode
and once against the actual belt. The acceptance criterion is the actual-belt
trace: time from the lap boundary to the belt being within 0.2 km/h of target,
expected to fall from "several seconds" to ≲1 s. Those numbers belong in this
section when they exist.
