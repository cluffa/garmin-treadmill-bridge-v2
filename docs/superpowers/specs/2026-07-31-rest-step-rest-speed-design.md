# Rest steps drop the belt to a walk — design

**Date:** 2026-07-31
**Status:** approved
**Touches:** `core/workout_ctrl.c`, `core/workout_ctrl.h`, `test/host/test_workout_ctrl.c`,
`test/mock/wkt_decode.py`, `test/mock/test_wkt_decode.py`

## Problem

During a structured interval workout, the belt keeps running the work-interval
speed straight through the rest step. Nothing happens at the work→rest boundary.

The cause is in `decode_action()` (`core/workout_ctrl.c`). Rest steps *do* reach
the bridge — the 2026-07-31 hardware bring-up recorded them arriving as
`intensity=1(rest) targetType=2(OPEN) flags=0x00`. Two guards send that frame to
`ACT_NONE`:

```c
if (!(flags & FLAG_HAS_STEP)) return ACT_NONE;              /* rest hits this one */
if (target_type != WORKOUT_STEP_TARGET_SPEED) return ACT_NONE;
```

`ACT_NONE` means "don't touch the belt", so `workout_ctrl_tick()` goes on
re-asserting the last latched work speed. That was a deliberate choice — it
avoids a slow belt restart on an OPEN rest step — but the result is a rest
interval you have to run at work pace.

## Behaviour

On a frame where the activity timer is running:

1. If a **usable speed target** resolves (`flags` bit0 set, `targetType == SPEED`,
   and the low/high midpoint is non-zero), command it. An explicit target always
   wins, including on a rest step.
2. Otherwise, if `intensity == WORKOUT_INTENSITY_REST (1)`, command
   **`REST_SPEED_KMH` = 4.0 km/h**.
3. Otherwise return `ACT_NONE` — hold the belt where it is.

`timerState != ON` still short-circuits to `ACT_STOP` before any of this.

4.0 km/h is commanded unconditionally, not as a floor. If a workout's work
interval were slower than 4 km/h, the rest step would speed the belt up. That is
accepted: tracking the last work speed to clamp it is state this module does not
otherwise need, and sub-4 km/h work intervals are not a real case here.

`REST_SPEED_KMH` is a compile-time constant in `core/workout_ctrl.c`. No new
wire field, no new ctrl-grammar command. Changing it needs a rebuild and a DFU
flash.

### What does *not* change

- **A free run still does not move the belt.** A free-run frame carries
  `intensity = 0xFF`, which is not `REST`, so it falls through to `ACT_NONE`
  exactly as before. The `CLAUDE.md` invariant holds.
- **An active step with a non-speed target still holds.** `intensity = 0(active)`
  with an HR/power/cadence/OPEN target is unchanged: the belt keeps its speed.
- Dedup, the 30 s keepalive, `ACT_STOP` on pause/stop, and
  `workout_ctrl_note_manual()` are all untouched. The rest step produces an
  ordinary `ACT_SPEED` latch, so the keepalive re-asserts 4.0 km/h during rest
  the same way it re-asserts a work speed.

### Known imprecision

The bring-up log recorded warmup and cooldown steps *also* arriving with
`intensity=1(rest)`, so in practice they will get 4.0 km/h too. A 4 km/h walk
warmup and cooldown is reasonable, so this is accepted rather than worked
around. If the watch is later found to report the real
`WARMUP(2)`/`COOLDOWN(3)` constants, the rule stays correct — those simply keep
holding.

## Implementation

`decode_action()` is restructured so the "resolve a speed target" path is one
block that either returns `ACT_SPEED` or falls through, rather than three
separate `return ACT_NONE` guards:

```c
#define WORKOUT_INTENSITY_REST 1
#define REST_SPEED_KMH         4.0f

if (timer_state != TIMER_STATE_ON) return ACT_STOP;

if ((flags & FLAG_HAS_STEP) && target_type == WORKOUT_STEP_TARGET_SPEED) {
    uint16_t mmps = (low_mmps && high_mmps) ? (low_mmps + high_mmps) / 2
                                            : (low_mmps ? low_mmps : high_mmps);
    if (mmps) { *kmh = mmps * 0.0036f; return ACT_SPEED; }
}

if (intensity == WORKOUT_INTENSITY_REST) { *kmh = REST_SPEED_KMH; return ACT_SPEED; }

return ACT_NONE;
```

`intensity` is frame byte `[3]`, already documented in `workout_ctrl.h` and until
now read by nothing in the firmware.

## Testing

`test/host/test_workout_ctrl.c` gains an intensity parameter on its frame
builder (existing calls keep `ACTIVE(0)`, which is what `memset` already gave
them, so no existing assertion changes meaning). New cases:

| Case | Expected |
|---|---|
| Rest step, OPEN target, `intensity=1` | one `set_speed(4.0)` |
| Rest step, speed target 6.0 km/h | `set_speed(6.0)` — explicit target wins |
| Rest step, speed target with lo=hi=0 | `set_speed(4.0)` — falls back |
| Work 8.5 → rest → work 8.5 | three commands, 8.5 / 4.0 / 8.5 |
| Keepalive during rest | re-asserts 4.0, not the work speed |
| Active step, OPEN target, `intensity=0` | no command (hold) |
| Free run, `intensity=0xFF`, `target=0xFF` | no command |
| Paused during a rest step | `stop`, no speed |

The existing case labelled "Rest step with an OPEN target → hold" is relabelled
to "non-rest step" — it builds `intensity=0`, so it was never testing a rest
step.

`test/mock/wkt_decode.py::annotate()` describes frame *content*, not policy, so
its text stays accurate. Its docstring and the lo=hi=0 comment reference
`ACT_NONE` outcomes that no longer hold for rest frames; both get corrected, and
`test/mock/test_wkt_decode.py` keeps asserting the labels.

Gate: `make host-test` (currently 9 host suites + 4 mock suites + `check-uuid`,
all green) plus `make firmware` linking clean.

## Non-goals

- **No watch-side change.** The data field's display keeps showing `-- km/h` on a
  rest step. It packs raw step data and deliberately owns no belt policy; making
  it show 4.0 would duplicate the bridge's rule on the watch, where it would
  drift. `watch/` is untouched.
- No runtime tuning of the rest speed (no `RESTSPEED` ctrl command).
- No incline handling on rest steps.
- No change to the free-run invariant.
