# Watch side — Connect IQ projects

Vendored into v2 on 2026-07-29 from the old repo
(`/Users/alex/workspace/nrf52/garmin-treadmill-bridge`), branch
`integration/combined-fixes` @ `e22d74e`. The newest CIQ commit at that point was
`e209814` *"fix(ciq): fall back to info.currentWorkoutStep when
getCurrentWorkoutStep() returns null"* (2026-07-08).

**Why they live here now.** They used to live only in the old repo, so the
firmware's BLE contract and the watch code that depends on it sat in two
different repos with nothing able to observe them diverging. They diverged: v2
shipped the 128-bit service UUID as a sanitization placeholder while these apps
kept using the real one, and because CIQ discovers the bridge by *filtering on
that UUID*, the bridge was simply invisible to the watch — no error, no log, no
clue. `make check-uuid` now guards it, and that guard only works if both sides
are in one tree.

## The two projects

| Project | Type | Characteristics used | Purpose |
|---|---|---|---|
| `garmin_data_field/` | data field | `A6ED0001` + `A6ED0004` (write) | Sends the 15-byte workout telemetry frame so the belt follows the workout target. **The main product path.** |
| `garmin_ctrl_app/` | app | `A6ED0001` + `A6ED0002` (write) + `A6ED0003` (notify) | SCAN / CONNECT / STATUS picker UI over the ctrl grammar and `D`/`E`/`S` frames. |

The data field deliberately does **not** subscribe to `A6ED0003` — it only
writes. So a bridge log that shows a watch connecting with no
`ctrl_svc: notifications on` is correct behaviour for the data field, not a
fault.

Products in both manifests: `fenix8solar51mm`, `fr965`, `fr955`, `fr970`.
`minApiLevel` 3.2.0.

## ⚠ A free run will not move the belt — by design

This is the single most confusing behaviour here, and it has cost debugging time
twice. `core/workout_ctrl.c` `decode_action()`:

| Watch state | Frame | Bridge action |
|---|---|---|
| Timer off / stopped / paused | `timerState != 3` | **`ACT_STOP`** — belt stops |
| Running, no structured step | `timerState=3`, `flags=0x00` | **`ACT_NONE`** — belt left **exactly** as-is |
| Running, step with non-speed target | `tgtType != 0` | `ACT_NONE` — belt held (keeps it moving through an OPEN rest step) |
| Running, step with speed target | `tgtType=0`, `lo`/`hi` mm/s | **`ACT_SPEED`** at the midpoint |

`ACT_NONE` means "don't touch the belt", and `workout_ctrl_tick()` keeps
re-asserting the last latched speed. So during a **free run** the belt holds
whatever it was last told — which looks exactly like "the data field isn't
working" while everything is in fact correct.

**To actually drive the belt you need a structured workout with a speed target
loaded and started on the watch.**

Observed on hardware 2026-07-29 with a genuine free run:
```
ctrl_svc: wkt #1 timer=3 flags=0x00 intensity=255
ctrl_svc: wkt tgtType=255 lo=0 hi=0 mm/s
```
`timer=3` = running, `flags=0x00` = no step, `255` = the `0xFF` "not known"
sentinels. That is a healthy data field reporting a free run. `_packFrame()`
emits this base frame when both `Activity.getCurrentWorkoutStep()` and the
`info.currentWorkoutStep` fallback return null.

## Wire format

The 15-byte frame written to `A6ED0004` must stay in lockstep with
`core/workout_ctrl.h` — that header carries the authoritative layout and says so.
`FRAME_VERSION` / `WORKOUT_FRAME_VERSION` is **1**; `FRAME_LEN` /
`WORKOUT_FRAME_LEN` is **15**. Speed targets are **mm/s** on the wire. A frame
whose length or version does not match is dropped by the firmware (now logged as
`wkt … MALFORMED` rather than silently).

## Building

No `monkeyc` on `PATH`; invoke it from an installed SDK. Available here:

```sh
ls ~/Library/Application\ Support/Garmin/ConnectIQ/Sdks/
# connectiq-sdk-mac-8.4.1-… / 9.1.0-… / 9.2.0-…
SDK=~/Library/Application\ Support/Garmin/ConnectIQ/Sdks/connectiq-sdk-mac-9.2.0-2026-06-09-92a1605b2

cd watch/garmin_data_field
"$SDK/bin/monkeyc" -f monkey.jungle -o out/app.prg -y <your-developer-key.der> \
                   -d fenix8solar51mm
```

A developer key is required to build. Build outputs (`out/`, `build/`, `bin/`)
are git-ignored — they were deliberately not vendored.

## Sideloading

Requires **USB mass-storage mode** — there is no over-the-air path for a
developer build (only store-distributed apps install wirelessly). Connect the
watch by USB, then:

```
cp out/app.prg /Volumes/GARMIN/GARMIN/APPS/
```

Eject cleanly, then add the field to a run activity's data screen.

## A note on the ctrl app's picker

An earlier version of this file warned that `LIST` returns blank names and the
picker is unusable. **That was a misdiagnosis** — see `docs/HANDOFF.md` open
item 2. A treadmill puts its service UUID in the primary advert and its name in
the scan response, so the bridge's *log* showed `found ""` at first sight while
the list entry got the name moments later. Names reach `LIST` fine.

The only residual wrinkle is a narrow race: a `LIST` issued in the window between
the primary advert and the scan response can show one device nameless. Re-`LIST`
and it resolves.
