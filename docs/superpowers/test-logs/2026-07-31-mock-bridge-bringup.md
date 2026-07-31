# Mock bridge hardware bring-up — 2026-07-31

**Result: the mock works. The data field does not.**

The bring-up was supposed to walk a 7-row acceptance table. It got three rows in
and found the watch-side bug the mock was built to hunt, so the rest of the table
could not be exercised — not because the mock failed, but because **the data
field never once emitted a speed target during a real structured workout.**

## Setup

- Real watch, data field on a run-activity screen. Hardware bridge powered off.
- Mock: `make mock-bridge` at commit `6203b7e`.
- Workout: warmup → 5 × (5:55/mi work + rest) → cooldown. Stopped at the end.
- Note from the operator: there is no way to do a free run and a structured
  workout in the same activity, so the planned "free run first" row was not
  available. The pre-start frames (`timer=0`) cover the same ground.

## Acceptance table

| # | Row | Result |
|---|---|---|
| 1 | Watch connects | ✅ `LINK frames arriving` at `00:08:22.428` |
| 2 | Free run / no step → `no change` | ✅ (as the pre-start and inter-step frames) |
| 3 | Speed target → `ACT_SPEED <midpoint>` | ❌ **never occurred — see below** |
| 4 | Hold >30 s → `KEEP` re-assert | ⬜ not reachable: nothing was ever latched to re-assert |
| 5 | Pause → `ACT_STOP` | ⚠️ partial — see "Timer states" |
| 6 | Resume → `ACT_SPEED` | ⬜ not reachable, same reason as row 4 |
| 7 | End → stale reset | ⬜ not observed; run was `^C`-ed ~60 s after stop, inside the 120 s window |

Rows 4, 6 and 7 are blocked by row 3, not independently failing.

## The finding

Every one of the 13 frames carried `flags=0x00`. Not one carried `tgt=0(SPEED)`.
During a workout whose entire purpose is a 5:55/mi pace target.

The frames fall into two clearly alternating shapes:

```
#4  timer=3(ON) flags=0x00 intensity=1(rest)     tgt=2(OPEN)
#5  timer=3(ON) flags=0x00 intensity=255(unset)  tgt=255(unset)
#6  timer=3(ON) flags=0x00 intensity=1(rest)     tgt=2(OPEN)
#7  timer=3(ON) flags=0x00 intensity=255(unset)  tgt=255(unset)
```

Counting the shapes across the run:

| Shape | Frames | Count | Maps to |
|---|---|---|---|
| `intensity=2` | #1, #2 | — | warmup |
| `intensity=255, tgt=255` | #3, #5, #7, #9, #11 | **5** | **the 5 work intervals** |
| `intensity=1(rest), tgt=2(OPEN)` | #4, #6, #8, #10 | 4 | the rests between them |
| `intensity=3` | #12 | — | cooldown |

Five all-sentinel frames for five work sets. **The data field can see the warmup,
rest and cooldown steps — it reports their intensity and target type correctly —
and goes completely blind on exactly the steps that carry the speed target.**

Trace it through `DataFieldView._packFrame`:

- Rest steps reach `f[3] = wStep.intensity` (→ 1) and `f[4] = tt` (→ 2), then
  return at `if (tt != Activity.WORKOUT_STEP_TARGET_SPEED) return f;` without
  setting `flags`. Correct behaviour, correctly reported.
- Work steps leave **both** `f[3]` and `f[4]` at their `0xFF` sentinels. Only two
  paths produce that: the early `if (wStep == null) return f;` — meaning both
  `Activity.getCurrentWorkoutStep()` **and** the `info.currentWorkoutStep`
  fallback returned null — or the `catch` block, which returns a fresh
  `_newBaseFrame()`, discarding everything.

Those two are indistinguishable in the frame, and that ambiguity is now the
blocker. The `catch` path already does `System.println("DataFieldView _packFrame
error: …")`, which goes nowhere reachable on real hardware.

This fully explains the operator's earlier report that "moving to another
interval with another target pace didn't seem to always work". It never worked.
The belt was never commanded because no speed target was ever transmitted.

## Timer states

- `#1` at `timer=0(OFF)` correctly produced `ACT_STOP`.
- `#13` at `timer=1(STOPPED)` produced `no change` — correct, and worth
  understanding: `workout_ctrl.c` had already latched `ACT_STOP` from frame #1
  and deduplicates. Not a fault.
- The operator reports pausing; the frame says `STOPPED` (1), not `PAUSED` (2).
  Both map to the same belt action, so it is cosmetic here — but it means end-of-
  activity and pause are not distinguishable in this log. Worth a targeted retest.

## What the mock got right

- Frame decode matched the wire format on every frame.
- Link inference worked: `LINK` ~0.4 s after the first frame, `idle` heartbeats
  every ~10 s through gaps of up to 25 s without a false disconnect. The 35 s gap
  that killed the original `is_connected()` design is comfortably inside the
  window.
- `ACT_STOP` / `no change` came from `core/workout_ctrl.c` itself, so the
  "no change" verdicts here are the real bridge's verdicts.

## What the mock got wrong

`annotate()` labelled the rest-step frames `FREE RUN - no structured step`. They
are nothing of the sort — `intensity=1(rest) tgt=2(OPEN)` is a structured step
the field simply did not flag. In a log whose whole job is to answer "why isn't
the belt moving", that label points the reader at the wrong bug. Fixed: the
no-step case now splits on whether the step fields are all sentinels.

## Next steps, in order

1. **Disambiguate null-step from exception.** Both currently produce an identical
   all-`0xFF` frame. `flags` has seven spare bits and the firmware ignores unknown
   ones, so this is cheap and backward-compatible: set bit1 when the step lookup
   threw and bit2 when it returned null. Then one more run says which it is.
2. **If it throws:** find the property access that raises on a pace-target step.
   `targetValueLow`/`targetValueHigh` on a pace target are the first suspects.
3. **If it is null:** `Activity.getCurrentWorkoutStep()` and
   `info.currentWorkoutStep` both returning null *only* for speed/pace-target
   steps is strange enough to need a minimal reproduction — likely a CIQ
   system-version behaviour worth pinning down before working around.
4. Re-run this table once a speed target actually reaches the wire. Rows 3–7 are
   all still unproven.

## Caveat

`intensity` values 1/2/3 are read here as rest/warmup/cooldown from their
alignment with the workout structure, not from a firmware-pinned constant table —
only `SPEED=0` for targetType is grounded in this repo's own source
(`core/workout_ctrl.c:10`). The mapping is consistent with the workout that was
run, but treat it as inference.
