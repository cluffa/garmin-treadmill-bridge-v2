# nRF52840 Treadmill Bridge v2 — Claude Code Instructions

## Build & flash

```sh
# Host tests (core/ only, no firmware)
make host-test

# Pace/lag scoring of a recorded SDM:TGT workout (needs uv + a .FIT; NOT part
# of host-test). See docs/pace-lag-analysis.md.
make pace-test

# Connect IQ (watch). ciq-build regenerates source/BuildInfo.mc and THEN runs
# monkeyc -l 2; sideload only ever CONSUMES a build. See watch/README.md.
make ciq-build
make sideload

# Firmware (nRF52840 + S340)
# Build with the top-level convenience target:
make firmware
# Or invoke the sub-Makefile directly with explicit paths:
make -C firmware -j8 \
  GNU_INSTALL_ROOT=/Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/Users/alex/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include

# Flash — full procedure, wiring, and gotchas: docs/flashing.md
# No onboard debugger: SWD via a Pico/CMSIS-DAP + pyocd (NOT nrfjprog); USB via nrfutil.
# All flash/DFU targets CONSUME an existing build (they do not rebuild). Build first.
make flash-full                        # first-time: SD+app+bootloader+settings, chip erase (SWD)
make flash-app                         # ⚠ BROKEN — parks in the bootloader; use flash-full
make flash-sd                          # SoftDevice only, chip-erases (⚠ see below)
make dfu                               # signed USB-DFU package (consumes build)
make dfu-enter                         # kick running app into DFU (SWD, GPREGRET)
# No SWD probe attached? Write `DFU` to the USB console instead — same GPREGRET
# path, no probe needed. This is the working route on this board:
#   make usb-kick && printf 'DFU\n' > /dev/cu.usbmodemXXXX
# The bootloader keeps the same USB serial, so the tty node name does NOT change;
# check `ioreg -r -c IOUSBHostDevice -w 0 | grep "USB Product Name"` instead.
make flash-dfu SERIAL=/dev/cu.usbmodemXXXX   # push package over USB
```

⚠ `make flash-sd` chip-erases (`--erase chip`): it wipes the bootloader,
settings page, and UICR, leaving a board that is no longer USB-updatable.
Prefer `make flash-full` for first-time provisioning.

### First build on a new machine

A fresh clone needs two git-ignored files before it will build:

```sh
cp firmware/ant_network_key.h.example firmware/ant_network_key.h
cp firmware/ant_license.mk.example   firmware/ant_license.mk
```

Without `ant_network_key.h` the build fails with
`fatal error: ant_network_key.h: No such file or directory`.
Without `ant_license.mk`, `sd_ant_enable()` fails at runtime (the build
completes with a warning only). Signing DFU packages additionally needs
`dfu/dfu_private_key.pem` (see `dfu/dfu_private_key.pem.example`).

## Toolchain

Paths below are this machine's concrete values — adjust to your own setup.

- **Host C compiler:** cc/gcc/clang (for `core/` + `test/host/`).
- **Firmware ARM toolchain:** PlatformIO's GCC 7.2.1 (SDK-compatible), NOT Homebrew's 16.1.0:
  `GNU_INSTALL_ROOT=/Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/  GNU_VERSION=7.2.1`
- **nRF5 SDK 17.1.0:** `SDK_ROOT=/Users/alex/nRF5_SDK_17.1.0_ddde560`
- **S340 v7.0.1 API headers:** `S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include`
- **S340 hex:** `/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex` (S340 is NOT in the SDK's softdevice dir)
- **Bootloader hex:** `/Users/alex/workspace/nrf52/build/bootloader_usb_s340.hex`

## `core/` purity invariant

Every file under `core/` compiles for the host with **no SoftDevice/nRF/BLE/NimBLE/ESP includes**.
nRF glue lives only in `firmware/`. Verify with:
```sh
grep -rEl 'nrf|softdevice|ble_|host/ble|nimble|esp_' core/ --include=*.c --include=*.h
```
Must be empty (pure protocol constant names like `ble_` are fine only if they pull no nRF headers).

## Control contract (unchanged from old repo)

- BLE service: `A6ED0001-D344-460A-8075-B9E8EC90D71B`
  ⚠ Do **not** replace this base with a placeholder. The Garmin CIQ data field
  and ctrl app filter on the 128-bit service UUID, so a sanitized base makes the
  bridge invisible to the watch with no error to explain it. v2 shipped with a
  `…-2E7A-4E1D-9E3B-000000000000` placeholder here and in `mock_watch.py`, which
  silently broke watch compatibility until 2026-07-29.
- Char `A6ED0002` write: uppercase ctrl grammar (`SPEED`, `SCAN`, `CONNECT`, `STOP`, `LIST`, `STATUS`).
- Char `A6ED0003` notify: compact `D`/`E`/`S` frames (CIQ MTU is 23; notify payload <= 20 bytes).
- Char `A6ED0004` write: raw 15-byte little-endian workout frame -> `workout_ctrl`.

## One-connection-at-a-time invariant

The BLE central holds at most one treadmill link; connecting tears down the other protocol first
and suppresses auto-reconnect while the other is connecting/connected. Never reintroduce
simultaneous FTMS+iFit connections.

## ANT

- Device type 124 (Stride SDM), channel period **8134** — the profile allows
  only 8134 and 16268, and a real SDM sensor was measured at 8134 on
  2026-08-18. It was 8192 until then; see `docs/ant-logging.md`.
- Network key in git-ignored `ant_network_key.h` (placeholder builds; real key from thisisant.com).
- ANT eval license key passed by Makefile.
- The footpod keeps **its own clock**, integrated from the app_timer RTC in
  `firmware/ant_sdm.c` — never `treadmill.elapsed_s`, which is 0 on iFit and on
  any FTMS treadmill that omits flag bit 10. Page 1's fractional time byte is
  part of that: at ~4 Hz a whole-second clock repeats across three of every four
  pages, so a receiver differentiating against it divides by zero.
- ⚠ **The bridge cannot measure cadence and must not invent one.** It has no
  stride sensor; the watch's wrist cadence is correct and is what should end up
  in the recording. Capabilities **page 22** (slot 7 of the TX cycle) declares
  cadence unsupported so the watch falls back to the wrist — bit 4 of byte 1,
  clear. Never set it, and never touch bits 0-2 (time/distance/speed) without
  reading `docs/sdm-recording-analysis.md` §11: that byte can flag the
  product's core function unsupported with no visible error. As of 2026-08-11
  page 22 is flashed and **verified not to fix cadence** — the recording still
  shows `avg_running_cadence` 252 / `max` 0 on a re-paired footpod
  (`docs/sdm-recording-analysis.md` §12). Keep the page: the byte is correct and
  speed/distance are unharmed.
- **Page 70 requests are answered** (`core/ant_request.c`), and `STATUS` reports
  `"p70"` / `"p70_rej"` / `"p70_pg"`. **The watch asks for page 22 by name, at
  pairing** — observed 2026-08-11 as `"p70":2,"p70_rej":0,"p70_pg":22` after a
  footpod re-pair, which is why unsolicited broadcasting never worked. Requests
  arrive at pairing only, so **a re-pair discards the answered state**: don't
  re-pair while a recording experiment is pending (`docs/sdm-recording-analysis.md`
  §13.5).
  ⚠ A page-70 response displaces the rotation page for its slot, so responses
  are spread one per four slots and capped; don't remove either without reading
  §13.2, or a request burst reintroduces the speed holes §10a fixed. And the ANT
  event handler must keep pumping on `EVENT_TRANSFER_TX_COMPLETED`/`_FAILED` as
  well as `EVENT_TX` — an acknowledged response replaces `EVENT_TX`, so dropping
  those two stalls the whole broadcast (§13.3).

## Logging another ANT+ sensor

`ANTLOG` (console) + `tools/ant_logger.py` open a **second** ANT channel — a
slave, channel 1 — that locks onto some other ANT+ sensor and logs every page it
receives to `logs/antlog-*.jsonl`, so a real sensor and our synthesized footpod
are captured by one board off one clock. `docs/ant-logging.md` has the usage,
the two non-obvious constraints (low-priority search, and the asynchronous
channel close that makes a naive re-open fail with 0x4015), and the first
capture's findings — including that a real treadmill SDM sensor broadcasts a
flat-zero time field and stride count, and a *valid* cadence.

Off unless asked for: an open slave channel costs radio time that belongs to
the footpod broadcast the watch is recording.

## Secrets (never committed)

- `ant_network_key.h` — see `ant_network_key.h.example`.
- `ant_license.mk` — see `ant_license.mk.example`.
- `dfu/dfu_private_key.pem` — see `dfu/dfu_private_key.pem.example`.

## Architecture

`core/` is platform-agnostic protocol logic (parsers, FSM, belt-control policy, ANT SDM encoding).
`firmware/` is the nRF5-SDK + S340 radio glue: BLE peripheral (watch-facing ctrl-svc), BLE central (treadmill-facing FTMS/iFit), ANT master (footpod broadcast), USB-CDC console.
The bridge between them is `core/machine.h` — a unified facade that auto-detects FTMS (0x1826) and iFit (0x1533) into one device list and routes connect/speed/incline/stop to the right adapter.
`firmware/app_state.h` is the shared struct all three radios and the testboard render from.
`watch/` is the Connect IQ side, vendored in on 2026-07-29: `garmin_data_field`
(writes the 15-byte workout frame to `A6ED0004` — the main product path) and
`garmin_ctrl_app` (the SCAN/CONNECT picker over `A6ED0002`/`0003`). See
`watch/README.md`.

`watch/*/source/BuildInfo.mc` is **generated** by `tools/ciq_stamp.sh` (checked
in, so a fresh clone compiles) and the data field renders it on its bottom row —
`CONN 0803-1901`. Always stamp before compiling, and **read the stamp off the
watch before trusting a before/after result**: a sideload that silently did not
take looks exactly like one that did, and debugging code that was never on the
device has cost this project a session already.

Firmware has the same guard: `firmware/build_stamp.c` is generated by
`tools/fw_stamp.sh` and reported by the console's `STATUS` as
`"build":"0811-1656 eed970d+"` (build time, git rev, `+` = uncommitted
changes). **Read it before trusting a before/after on hardware** — a DFU that
did not take also looks exactly like one that did. Unlike `BuildInfo.mc` it is
**git-ignored**: firmware only builds through the Makefile, which regenerates it
at parse time, and tracking it would leave the tree permanently dirty and so pin
its own `+` marker on forever.

⚠ **A free run does not move the belt, by design.** `decode_action()` returns
`ACT_NONE` when no structured workout step is present, which means "don't touch
the belt", and `workout_ctrl_tick()` keeps re-asserting the last latched speed.
*Starting* the belt requires a structured workout with a **speed** target. This
has looked like a bug twice; it isn't.

## Difficulty scaling

Every workout-derived target is commanded as `base * scale`, where `base` is
the step target as authored and `scale` is learned from one speed adjustment
the user makes on any interval (`core/workout_ctrl.c`, spec in
`docs/superpowers/specs/2026-08-12-workout-difficulty-scaling-design.md`).
Three things are load-bearing and easy to "simplify" wrongly:

- **A ratio outside [0.5, 1.5] is discarded, never clamped.** A pulled safety
  key reads as ~0 km/h; clamping it into range restarts the next interval at
  half pace with nobody having asked.
- **The belt detector arms on *reach*, not on a settle timer.** No constant
  covers both a 0.2 km/h nudge and a 4→12 km/h iFit ramp, and arm-on-reach is
  also what stops a treadmill's own speed ceiling being read as a user
  adjustment.
- **`workout_ctrl_reset()` does not reset the scale** — it fires on watch
  disconnect, and a CIQ BLE blip mid-run must not undo the user's adjustment.
  Only `TIMER_STATE_OFF` (a fresh activity) and a reboot reset it.

Read it on hardware from `STATUS` (`"scale":100`) or testboard row 6.

The one step that moves the belt without a speed target is a **rest** step
(`intensity == WORKOUT_INTENSITY_REST`), which is commanded to
`REST_SPEED_KMH` (4.0) so intervals walk out the rest instead of holding work
pace. It is a plain unconditional set, not a floor: keep the free-run and
active-step paths on `ACT_NONE`, or a free run starts moving the belt.

## Belt response lag

`test/pace_lag_report.py` (`make pace-test`) grades a recorded SDM:TGT workout:
it rebuilds what `workout_ctrl.c` should have commanded from the .FIT's own
workout steps and laps, and scores the recorded trace against it — response lag,
area between the curves, and periodic ANT speed dropouts. It carries a Python
mirror of `decode_action()` and parses `REST_SPEED_KMH` out of
`core/workout_ctrl.c` plus `CYCLE_LEN`/`SDM_CHANNEL_PERIOD` out of
`firmware/ant_sdm.c`, so **changing any of those three changes the scorer's
model** — that is deliberate, but re-scoring an *older* .FIT then needs the
recording firmware's values passed explicitly (`--sdm-cycle-s`,
`--rest-policy`). Findings and the current numbers: `docs/pace-lag-analysis.md`.

The default gate is the **post-fix** trace (`test/23842067586_ACTIVITY.fit`,
2026-08-03) with `SDM_CYCLE` **pinned to 16.0** — that trace was recorded when
`SDM_CHANNEL_PERIOD` was 8192, and the tree now transmits at the
profile-correct 8134, so reading the cycle out of `ant_sdm.c` would model the
trace with a period its recording firmware never used. Put the pin back to
empty when the default trace is re-recorded; until then a *new* .FIT scored
with `make pace-test FIT=…` silently inherits 16.0 and must override it.
Re-scoring the archived pre-fix trace needs all three overrides together —
`FIT=`, `BASELINE=`, and `SDM_CYCLE=17.0`; the exact line is in the Makefile
comment above the vars. Both .FITs are **untracked on purpose** (real
activities: HR, timestamps, device serial), so `make pace-test` fails on a fresh
clone until one is supplied.

`make host-test` runs `make check-uuid` first, which asserts firmware, mock, and
both CIQ projects agree on the 128-bit A6ED base. Keep it that way: the watch
finds the bridge by *filtering* on that UUID, so any disagreement is silent — the
watch simply never sees the device.

## `firmware/` Makefile TESTBOARD stamp

The Makefile auto-detects `TESTBOARD` changes between builds and force-cleans objects.
Don't be surprised by a clean rebuild when you toggle between `TESTBOARD=0` and `TESTBOARD=1`.

## Source of truth for vendored files

The old repo at `/Users/alex/workspace/nrf52/garmin-treadmill-bridge/` (referred to as `$OLD`).

## Finishing plan

All code milestones (M0-M4) are complete as of `fc5ac15`. The only remaining step is the
M4.2 three-radio concurrency gate on physical hardware. See `docs/finishing-plan.md`.
