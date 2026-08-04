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

From the repo root:

```sh
make ciq-build                          # data field (main product path)
make ciq-build CIQ_PRJ=garmin_ctrl_app  # the picker
```

That regenerates `source/BuildInfo.mc` and *then* runs `monkeyc -l 2`, in that
order. Override `CIQ_SDK` / `CIQ_KEY` / `CIQ_DEV` if your paths differ.

### The build stamp

`source/BuildInfo.mc` is **generated** by `tools/ciq_stamp.sh` and holds a
single `MMDD-HHMM` constant. The data field renders it on its bottom row next to
the link state — `CONN 0803-1901` — so the watch itself reports which build is
running.

This exists because a sideload that silently did not take, or an install the
watch skipped on eject, is otherwise indistinguishable from a working push: the
field looks identical either way, and you end up debugging code that was never
on the device. Read the stamp before trusting any before/after result.

The file is **checked in**, not git-ignored, so a fresh clone still compiles
with a plain `monkeyc` invocation. The one-line churn per build is deliberate —
it records what was built. Always stamp before compiling; `make ciq-build` does,
and `tools/ciq_sideload.sh` hard-errors when a source file is newer than the
`.prg`.

### By hand

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

There is no over-the-air path for a developer build (only store-distributed
apps install wirelessly) — sideloading means copying the `.prg` onto the watch
over USB. **The fenix 8 has no USB mass-storage mode**: `/Volumes/GARMIN` never
mounts, so the old `cp out/app.prg /Volumes/GARMIN/GARMIN/APPS/` recipe is dead
on this watch. It speaks **MTP** instead.

### Prerequisites

- `libmtp` tools (`mtp-detect`, `mtp-filetree`, `mtp-sendfile`, …) — installed
  on this machine at `/opt/homebrew/bin` (`brew install libmtp` if missing).
- Watch connected by USB. Verify with `mtp-detect` — you should see
  `Garmin: Fenix 8 Solar Sapphire`.

### Procedure

In the repo root the whole flow is one command (consumes the existing build;
`CIQ_PRJ=garmin_ctrl_app` for the picker once that project is built):

```sh
make sideload
```

The mechanics live in `tools/ciq_sideload.sh` (the Makefile just calls it —
every MTP call needs a timeout, which is too much shell to keep legible in a
recipe). Knobs:

| Env | Effect |
|---|---|
| `SIDELOAD_FORCE=1` | send even though sources are newer than the `.prg` |
| `SIDELOAD_CHECK_DUPES=1` | opt into the slow duplicate-`app.prg` scan (off by default — see pitfalls) |
| `SIDELOAD_LOOKUP_TIMEOUT` | seconds for the folder-id lookup (default 60) |
| `SIDELOAD_SEND_TIMEOUT` | seconds for the transfer (default 240) |

The staleness check runs **before** the transfer and is a hard error: pushing a
`.prg` older than your edits and then debugging the old build on the watch is a
trap worth failing loudly on.

What it does, step by step (also useful when doing it by hand or verifying):

```sh
# 1. Find the Apps folder id (the numeric id under GARMIN; it can vary between
#    devices, so always look it up rather than hardcoding):
mtp-filetree
#    16777216 GARMIN
#      16777227 Apps        <-- this id (was 16777227 on 2026-07-31)

# 2. Send the built app by NUMERIC folder id (see pitfalls for why):
mtp-sendfile out/app.prg 16777227
#    ... Sending file...
#    New file ID: 16779871

# 3. Verify it landed at the Apps level (NOT inside Apps/DATA):
mtp-filetree | grep -i app.prg
```

Then unplug the watch — it scans `GARMIN/Apps` on eject/boot and installs the
app — and add the field to a run activity's data screen. If the watch already
has a copy of the app installed, the sideload updates it.

### Pitfalls (observed 2026-07-31, plus the hang/wedge entries 2026-08-02)

- **`mtp-sendfile` (libmtp 1.1.23) has no `-f` folder flag.** `-f 16777227` is
  parsed as the *local* filename and dies with `-f: stat: No such file or
  directory` before anything transfers.
- **Path destinations fail.** `mtp-sendfile out/app.prg /GARMIN/Apps/app.prg`
  errors with `Parent folder could not be found` — the name walk
  (`lookup_folder_id` in libmtp's `pathutils.c`) does not match Garmin's tree.
  Pass the bare numeric folder id instead; `parse_path` treats a number as an
  item id directly.
- **`GARMIN/Apps/DATA/app.prg` is the previous install**, not a sideload
  target. A successful sideload sits at the Apps level as a sibling of
  `OUT.BIN`.
- **Re-runs can leave duplicate `app.prg` files.** Garmin's MTP delete is
  unreliable (PTP error 2002) and its object enumeration is stale (`mtp-files`
  can report ids `mtp-filetree` no longer shows), so `make sideload` does not
  auto-remove the previous copy. The watch installs the newest copy (same app
  id), so duplicates are dev-noise, not corruption. To clean up:
  `mtp-delfile -n <older-id>` and retry if it errors.
- **The duplicate scan is off by default** (2026-08-02). `mtp-files` walks every
  object on the device, and it was measured hanging **~4 minutes at 0.0% CPU**
  before the transfer had even started — all to print one warning that changes
  nothing, since nothing is auto-removed either way. `SIDELOAD_CHECK_DUPES=1`
  brings it back, now under a timeout and non-fatal.
- **Garmin's MTP blocks instead of erroring.** With no watch attached,
  `mtp-filetree` does not print "no raw devices" — it hangs. Every MTP call in
  `ciq_sideload.sh` is therefore wrapped in a timeout, and a sessionless
  `ioreg` vendor-id check (0x091e) short-circuits the not-plugged-in case
  without opening a session at all.
- **Killing an MTP command mid-session wedges libmtp.** Subsequent calls fail
  with `PTP_ERROR_IO: failed to open session` / `LIBMTP PANIC: failed to open
  session on second attempt`, and the USB-interface reset libmtp attempts on
  its own does **not** clear it — only a physical unplug/replug does. The
  script detects this signature and says so instead of letting you retry into
  the same wall. Note a watch can be enumerated in `ioreg` (right VID:PID) and
  still refuse to open a session — a partially-enumerated device shows no
  `USB Product Name` in the USB tree.
- **Don't pipe `mtp-sendfile` into `grep -q`.** `grep -q` exits at the first
  match and closes the pipe, which can SIGPIPE the transfer mid-write. The
  script captures output to a file and greps that.
- Only `garmin_data_field` has been built in this repo
  (`watch/garmin_data_field/out/app.prg`); `garmin_ctrl_app` needs its own
  `monkeyc` build before it can be sideloaded.

## A note on the ctrl app's picker

An earlier version of this file warned that `LIST` returns blank names and the
picker is unusable. **That was a misdiagnosis** — see `docs/HANDOFF.md` open
item 2. A treadmill puts its service UUID in the primary advert and its name in
the scan response, so the bridge's *log* showed `found ""` at first sight while
the list entry got the name moments later. Names reach `LIST` fine.

The only residual wrinkle is a narrow race: a `LIST` issued in the window between
the primary advert and the scan response can show one device nameless. Re-`LIST`
and it resolves.

## Debugging the data field without the hardware bridge

`make mock-bridge` runs a macOS BLE peripheral (`test/mock/mock_bridge.py`) that
advertises the same `A6ED0001` control service as the firmware. The data field
cannot tell it apart from the real bridge, so you can iterate on watch code with
the nRF52840 out of the loop entirely.

It logs every frame written to `A6ED0004`, decoded, along with the belt command
the firmware would actually have issued — that prediction comes from
`core/workout_ctrl.c` itself, compiled to `libworkout_probe.so` and loaded
through ctypes, so it cannot drift from the bridge.

```
21:04:31.882  ADV   TMILL-MOCK  A6ED0001-D344-460A-8075-B9E8EC90D71B
21:04:33.104  WKT   #1  len=15 ver=1 timer=3(ON) flags=0x01 intensity=0(active)
                        tgt=0(SPEED) lo=2222 hi=2500 mm/s (8.0-9.0 km/h) dur=5 300 rep=0
                        -> ACT_SPEED 8.5 km/h   [speed step]
21:04:33.912  LINK  frames arriving - watch attached (inferred from traffic; bless is_connected() is subscription-based and the data field never subscribes)
21:04:43.912  idle  (no write - field sends on change only)
21:04:53.912  idle  (no write - field sends on change only)
21:05:02.912  KEEP  -> re-assert 8.5 km/h
```

Arithmetic behind that block, for a 1 Hz loop, `IDLE_LOG_S=10`, `KEEPALIVE_TICKS=30`:
the mock's tick loop runs `asyncio.sleep(1.0)` in a plain `while True`, so once it
settles into a phase it prints on that phase every second — `:33.912`,
`:34.912`, `:35.912`, … here. `LINK` fires on the first tick at/after the write
(`:33.912`) and resets the "last logged" clock to that tick. `idle` then fires
every tick where 10.0 s have elapsed since the last logged line:
`33.912 + 10.0 = 43.912`, then `43.912 + 10.0 = 53.912` — **two** `idle` lines,
not one, before the keepalive. The keepalive counter (`s_ka_ticks` in
`core/workout_ctrl.c`) starts at 0 on the same write and increments by 1 on
every subsequent tick regardless of link state; it reaches `KEEPALIVE_TICKS=30`
on the 30th tick after the write, i.e. `33.912 + 29 * 1.0 = 62.912` (the first
post-write tick is the 1st increment, so it's `+29`, not `+30`, more ticks
later) — `21:05:02.912`, one tick earlier than `21:05:03.926` in the version
this replaces. That KEEP tick also preempts what would otherwise have been a
third `idle` line at `63.912`: the loop checks the keepalive before the idle
heartbeat on every tick, so only one of the two ever prints.

Note that `LINK` is **inferred from write traffic**, not read from a BLE
connection callback: bless's `is_connected()` reports subscribed centrals, and
the data field never subscribes to anything (it only writes), so that API is
always `False` here. The mock instead watches for a write to **any** of its
characteristics — `on_write()` records the write timestamp before it looks at
which characteristic was written, so a write to `A6ED0002` counts too, not
just `A6ED0004` — and declares the link stale after 120 s of silence on all of
them — deliberately generous, since a steady free run can legitimately go tens
of seconds between writes (the field sends on change, not on a timer).

⚠ **Power the real bridge off first.** The data field pairs with the first
device it finds advertising the service UUID; with both on air you will be
debugging the wrong peer.

A quiet log is usually correct — the field only writes when the frame changes,
which is why an `idle` heartbeat prints every 10 s while connected.

It does **not** emulate the ctrl grammar (`A6ED0002`/`0003`), so
`garmin_ctrl_app` is not exercised by it.
