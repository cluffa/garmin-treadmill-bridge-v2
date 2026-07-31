# 2026-07-31 — speed-target frames: root cause found and fixed

## Outcome

The "data field never emits a speed target" blocker is resolved. Speed targets
reach the wire **complete and clean** (`flags=0x01`), the belt command fires
(`ACT_SPEED`), and the full acceptance table now passes against the mock.

## Root cause

`_packFrame()` threw an exception **after** building an otherwise-complete
frame. The old `catch` discarded the frame and returned a fresh base frame, so
every speed step arrived as all-sentinel (`intensity=255 tgt=255`).

Diagnostic flag bits added in `DataFieldView.mc` (bits 1–3, ignored by
`workout_ctrl.c`, which only reads bit0) disambiguated the three silent paths
on the first instrumented run:

| flags | Meaning | Observed |
| `0x04` | no workout step resolved (both accessors null) | at connect / after workout end — correct |
| `0x03` | HAS_STEP + pack threw | **every speed step** |
| `0x00` | resolved, non-speed target (rest/warmup/cooldown) | correct |

`0x03` with `lo/hi` populated proved the throw came after the target bytes were
written. The only field never written was `durationValue`. Second run with a
new bit `0x10` + coercion fix:

- `wStep.durationValue` is a **Long** on pace-target steps; `_u32()`'s
  `v & 0xFF` then assigns Long→Byte, which throws in Monkey C.
- Fix: `wStep.durationValue.toNumber()` before packing.
- After fix: `flags=0x01` clean on every speed step, `dur` value now written.

## Acceptance table (mock run, easy recovery workout)

| Row | Scenario | Result |
| 3 | speed target → `ACT_SPEED` | **PASS** — 8.4 km/h (2055–2611 mm/s midpoint) |
| 4 | 30 s keepalive re-assert | **PASS** — 4× KEEP at 30 s, latch held |
| 5 | pause → `ACT_STOP` | **PASS** — timer=1(STOPPED) |
| 5 | resume → re-command | **PASS** — timer=3(ON), same step, `ACT_SPEED` |
| 6 | end-of-activity | **PASS** — timer=0(OFF), flags=0x04, belt untouched |

## Notes

- Pause reports `timer=1(STOPPED)` on this watch, never `2(PAUSED)`. Cosmetic —
  both map to `ACT_STOP`.
- The 120 s mock LINK-stale fired mid-steady-run (no writes for 2 min) as
  designed; do not tighten the window.
- End-of-activity emits a duplicate frame via `onTimerStop`'s forced push —
  harmless (dedup is bypassed by design for timer transitions).

## Evidence

Raw mock console: `15:40:07`–`15:43:31`, frames #1–#7 (see session transcript
or the tmux pane log). Frames #2/#4: `flags=0x01 tgt=0(SPEED) lo=2055 hi=2611
dur=0 2220` → `ACT_SPEED 8.4 km/h`.

## Files changed

- `watch/garmin_data_field/source/DataFieldView.mc` — FLAG_SRC_* bits, keep
  partial frame in catch, `dv.toNumber()` fix
- `core/workout_ctrl.h` — wire-format comment documents bits 1–4
- `test/mock/wkt_decode.py` + `test_wkt_decode.py` — annotate() names the bits
- `docs/HANDOFF.md` — current state rewritten
