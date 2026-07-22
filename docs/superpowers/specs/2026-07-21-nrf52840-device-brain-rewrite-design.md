# nRF52840 Treadmill Bridge — Ground-Up Rewrite ("Device is the Brain")

**Date:** 2026-07-21
**Status:** Approved design, pre-implementation
**Repo:** `garmin-treadmill-bridge-v2` (new standalone, nRF52840-only)
**Supersedes:** the `boards/xiao-nrf52840` variant in the old multi-board repo

## Problem

The old `garmin-treadmill-bridge` is a multi-board repo (ESP32-C6, Heltec, XIAO
nRF52840). Its treadmill-control path is proven on the ESP32 boards, but:

1. **The nRF52840 variant was never actually tested on hardware** — the S340
   platform glue (BLE central, BLE control service, ANT SDM master, `main.c`)
   is unproven. It may or may not work.
2. **The control flow relies too heavily on watch logic.** The Connect IQ data
   field parsed workout steps and decided target paces, making the watch side
   fragile and hard to test. The "desired flow" (belt-control policy on the
   device, watch ships raw telemetry) exists on a branch but does not work.
3. **You cannot test any of it without a real Garmin watch and a real
   treadmill.** There is no on-device visibility and no way to stand in for the
   watch or treadmill.

## Goal

A clean, standalone, **nRF52840-only** firmware where **the device is the
brain**: it owns all belt-control policy; the watch is reduced to a dumb speed
sensor (native ANT+) plus a raw-telemetry source (BLE). Plus a **bring-up/test
aid** (Seeed XIAO expansion board: OLED, button, buzzer, onboard RGB LED) and a
**host-side emulation harness** (fake watch + fake treadmill) so the whole
system can be exercised with no Garmin watch and no treadmill.

```
Treadmill ─BLE(FTMS/iFit)→ nRF52840 ─ANT+ (SDM footpod)→ Watch (native speed sensor)
                              ▲   └─BLE(GATT server "A6ED")── Watch data field (RAW telemetry)
                              └─ workout_ctrl DECIDES belt speed, keepalive, pause/rest
```

The nRF52840 wears three concurrent radio hats via the **S340 SoftDevice**: BLE
**central** (treadmill), BLE **peripheral** (control service), ANT **master**
(speed broadcast).

## Key decisions (locked)

| Decision | Choice | Rationale |
|---|---|---|
| Topology | Device is the brain | Watch makes zero belt decisions; ships the raw 15-byte workout frame. All policy in `workout_ctrl`. |
| Repo | New standalone nRF-only (`garmin-treadmill-bridge-v2`) | "From the ground up," one target, no ESP baggage. |
| Stack | nRF5 SDK 17.1.0 + **S340** v7.0.1 | Only concurrent BLE+ANT SoftDevice; matches Nordic's ANT+ SDM examples. |
| Treadmill protocols | Both FTMS + iFit | Preserve generic support, not just the owned NordicTrack 6.5S. |
| Protocol code | **Reuse the proven, host-tested bytes**; rebuild all nRF/S340 glue | Treadmill-side parsers/FSM/encoders were validated on ESP32 & host tests; the radio glue was never tested. |
| Data field | **Out of scope this cycle** (firmware-first) | Keep the existing 15-byte `A6ED0004` frame as the decode contract; rewrite the CIQ app later. |
| Expansion board | **Bring-up/test aid only**, compile-time optional | A debug dashboard so you don't need the watch to test; no-op when absent. |
| Flash/debug | **USB-DFU** (Nordic secure bootloader / Adafruit `nrfutil`) + **USB-CDC** logs | No J-Link for everyday work; logging must work with no probe. SWD/RTT optional for deep bring-up. |
| Emulation | Host BLE mocks (fake watch + fake treadmill) primary; **Renode = bounded non-radio spike** | Renode cannot run the S340 radio (see Testing). |

## Repository layout

```
garmin-treadmill-bridge-v2/
  core/                       # platform-agnostic, host-tested — VENDORED, re-verified
    model.h                   #   treadmill_state_t
    ftms_parse.{c,h}          #   FTMS frame decode        (proven)
    ifit_parse.{c,h}          #   iFit frame decode        (proven)
    ifit_fsm.{c,h}            #   iFit init/keepalive FSM + frame builders  (== old ifit_poll.c, host-tested)
    ctrl_dispatch.{c,h}       #   uppercase grammar: SPEED/SCAN/CONNECT/STOP (proven)
    ctrl_frames.{c,h}         #   compact D/E/S notify frames               (proven)
    ftms_devlist.{c,h}        #   merged FTMS+iFit device list              (proven)
    connect_policy.{c,h}      #   saved-device-first / strongest-RSSI       (proven)
    workout_ctrl.{c,h}        #   THE BRAIN: decode watch frame -> belt speed policy (proven)
    ant_sdm_encode.{c,h}      #   treadmill_state -> 8-byte SDM pages       (host-tested, radio-unverified)
  firmware/                   # NEW nRF5 SDK 17.1.0 + S340 platform layer
    main.c                    #   bring-up: SoftDevice/BLE/ANT init, scheduler, wiring
    ble_central.{c,h}         #   S340 GATT client: scan FTMS(0x1826)/iFit, connect, notify, ctrl writes
    ble_ctrl_svc.{c,h}        #   S340 GATT server: A6ED service; watch frame -> workout_ctrl
    ant_sdm.{c,h}             #   ANT master: SDM footpod (device type 124), broadcast SDM pages
    app_state.{c,h}           #   single owned app state; the sink all three radios feed / read
    usb_cdc_log.{c,h}         #   USB-CDC console: logs + ctrl grammar (works with no probe)
    testboard/                #   expansion-board test aid — compile-time optional (TESTBOARD)
      testboard.{c,h}         #   state -> OLED render; events -> LED/buzzer; button -> test actions
      ssd1306.{c,h}           #   I2C OLED driver (128x64)
      hw_button.{c,h}         #   debounced button (GPIOTE)
      hw_buzzer.{c,h}         #   PWM buzzer tones
      hw_led.{c,h}            #   onboard RGB LED (status)
    board_pins.h              #   XIAO nRF52840 + expansion-board pin map
    app_config.h              #   all SDK overrides (USE_APP_CONFIG)
    sdk_config.h              #   SDK template (from multiprotocol/ble_ant_app_hrm s340 config)
    ant_network_key.h(.example)  # git-ignored real key; placeholder builds
    xiao_nrf52840_s340.ld     #   linker: S340 APP_CODE_BASE / RAM origin
    Makefile                  #   build + dfu packaging + flash targets
  test/
    host/                     # C unit tests for core/ (extend old suite; must stay green)
    mock/                     # host BLE emulation harness (Python, self-contained uv scripts)
      mock_treadmill.py       #   FTMS/iFit peripheral the firmware's central connects to
      mock_watch.py           #   BLE central that writes A6ED workout frames (fake data field)
      README.md
    renode/                   # bounded spike: boot + non-radio peripheral sim, SoftDevice stubbed
  dfu/                        # DFU keys (private git-ignored) + bootloader hex + packaging notes
  docs/superpowers/specs/     # this spec + the implementation plan
  README.md  CLAUDE.md  LICENSE  .gitignore  Makefile
```

Everything under `core/` compiles for the host with **no SoftDevice/nRF
includes** — the invariant that keeps it host-testable. All nRF glue lives in
`firmware/`.

## `core/` reuse boundary

**Vendored unchanged (re-verified by host tests):** `model.h`, `ftms_parse`,
`ifit_parse`, `ifit_fsm` (old `boards/xiao-nrf52840/ifit_poll.c`, host-tested as
`test_ifit_poll`), `ctrl_dispatch`, `ctrl_frames`, `ftms_devlist`,
`connect_policy`, `workout_ctrl`, `ant_sdm_encode`.

**Rebuilt fresh (were untested nRF/S340 glue, used only as reference):**
`ble_central`, `ble_ctrl_svc`, `ant_sdm`, `main`, `sdk_config.h`, linker,
Makefile. The old `platform_ble_central.c` (783 lines) etc. are read as a
starting point and validated line-by-line, not trusted.

**Dropped:** everything ESP/NimBLE (`machine*.c` transport layers, `garmin_rsc`,
`rsc_encode`, `ctrl_svc.c`'s NimBLE bits, `serial_ctrl`). The iFit FSM logic
survives as `ifit_fsm`; the ESP transport does not.

## Firmware components

### `app_state` — single source of truth
One owned struct: latest `treadmill_state_t`, active treadmill link + protocol,
watch-connection state, latest resolved workout target, fault/status. The three
radios and the test board read/write it through a small API (no globals sprayed
across modules). This is the seam the test board renders and the mocks exercise.

### `ble_central` — treadmill link (BLE central)
S340 GATT client. Scans FTMS (`0x1826`) and iFit (`0x1533`), builds the merged
device list (`ftms_devlist`), connects per `connect_policy` (saved-device-first,
else strongest RSSI after a 6 s window; 15 s hold-out when a saved device exists
but isn't visible). Subscribes to notifications, pumps frames into
`ftms_parse`/`ifit_parse` → `app_state`. Owns control writes; iFit writes are
injected at the correct keepalive phase — **timing owned by `ifit_fsm`, now
driven off S340 events, not a NimBLE poll loop.** Preserves the
**one-connection-at-a-time invariant** (tear down the other protocol before
connecting; suppress auto-reconnect while the other is connecting/connected).

### `workout_ctrl` — the brain (in `core/`)
Decodes the watch's raw 15-byte telemetry frame (target/duration/intensity +
Activity timer state), resolves the belt-speed target (interval work/rest via
intensity), commands the treadmill on change immediately, re-asserts on a ~30 s
keepalive (`workout_ctrl_tick()` ~1 Hz), stops the belt on timer pause/stop,
keeps the belt moving through interval rest. **Every belt decision lives here.**
Both the BLE control service (real watch) and the test board / mock watch feed
it identically.

### `ant_sdm` — speed relay (ANT master)
ANT master channel advertising as an ANT+ Stride-Based Speed & Distance Monitor
(device type 124). Periodically transmits SDM pages built by `ant_sdm_encode`
from the latest `treadmill_state_t`. Uses the ANT+ network key. The watch pairs
it as a native footpod — speed/pace with no BLE slot used.

### `ble_ctrl_svc` — control receive (BLE peripheral / GATT server)
S340 GATT server exposing the `A6ED0001-…` service:
`0002` write (uppercase ctrl grammar → `ctrl_dispatch`), `0003` notify (compact
D/E/S frames), `0004` write (raw 15-byte workout telemetry → `workout_ctrl`).
Advertises RSC `0x1814`-free; carries the A6ED 128-bit UUID in the scan response
so CIQ can find it. Single peripheral link (`NRF_SDH_BLE_PERIPHERAL_LINK_COUNT=1`).

### `usb_cdc_log` — probe-free console
USB-CDC ACM: `NRF_LOG` backend + an interactive line reader wired to
`ctrl_dispatch` (SCAN/LIST/CONNECT/SPEED/STOP/STATUS). This is the everyday
debug channel since there's no J-Link. Pushes `{"event":"state",…}` lines.

### `testboard/` — bring-up/test aid (compile-time optional)
Behind `#if TESTBOARD`; compiles to no-ops when the expansion board is absent.
- **OLED (SSD1306, I2C):** BLE-central/ANT/ctrl status, belt speed, resolved
  target, current workout step, last error/fault.
- **Onboard RGB LED:** green = treadmill linked, blue = watch linked, red =
  fault (blend/priority when multiple).
- **Buzzer (PWM):** chirp on connect / target change / error.
- **Button (GPIOTE, debounced):** short-press cycles a test action
  (scan / connect-next / inject a fixed test target pace / stop); long-press
  resets. Lets you drive and observe the full brain with **no watch, no
  treadmill.**

### `main.c` — bring-up
SoftDevice enable (S340) → BLE + ANT stack init → app scheduler / timers →
`app_state` init → wire `ble_central`, `ble_ctrl_svc`, `ant_sdm`, `usb_cdc_log`,
`testboard` → run. `app_timer` drives the 1 Hz `workout_ctrl_tick` and the ANT
broadcast cadence.

## Concurrency model (top risk — same as before)

S340 multiplexes BLE central + BLE peripheral + ANT master over radio
timeslots. Budget committed and validated early:
- **Treadmill BLE central:** connection interval chosen to leave room for ANT +
  peripheral advertising while meeting the iFit keepalive cadence (~1.5 s poll;
  writes must land in their phase slots).
- **ANT+ SDM master:** standard SDM channel period (8192-count, ~4.06 Hz).
- **BLE peripheral (control):** low-duty advertising unconnected; one
  low-bandwidth link when the data field/picker is attached.

**Make-or-break gate (must pass before feature work):** bring all three radios
up concurrently on real hardware and confirm a belt-speed change still lands
(iFit keepalive timing survives S340 event latency + concurrent ANT traffic).
This is real-hardware-only; nothing below can substitute for it.

## Testing strategy (layered; honest about emulation)

1. **Host unit tests (`test/host/`, the trustworthy layer):** all of `core/`
   with no radio — known state → known bytes. Extend the existing suite
   (`test_ftms_parse`, `test_ifit_parse`, `test_ifit_poll`, `test_ctrl_dispatch`,
   `test_ctrl_frames`, `test_ftms_devlist`, `test_connect_policy`,
   `test_workout_ctrl`, `test_ant_sdm_encode`). **Gate: green before any
   firmware feature work.**
2. **Host BLE emulation (`test/mock/`, the real integration layer):**
   - `mock_treadmill.py` — FTMS/iFit peripheral the **real firmware's central**
     connects to; verifies scan/connect/parse/keepalive and that belt-speed
     commands actually land.
   - `mock_watch.py` — BLE central that connects to `ble_ctrl_svc` and writes
     A6ED workout frames (the fake data field); verifies "device is the brain"
     belt control end-to-end with **no Garmin watch**.
   Runs the actual firmware on real XIAO hardware over real BLE.
3. **ANT check:** read the SDM broadcast with the workspace ANT USB stick
   (`ANT-SDK_Mac.3.5`) or a second Garmin to confirm a valid footpod.
4. **Renode spike (`test/renode/`, bounded):** Renode's nRF52840 BLE model
   targets **Zephyr's open stack, not the S340 SoftDevice** — the SoftDevice
   binary needs the TEMP peripheral + RSSI/errata sampling Renode doesn't model,
   so S340 radio will not run. Use Renode only to prove the firmware **boots and
   drives non-radio peripherals** (timers, GPIO, the I²C OLED) with the
   SoftDevice calls stubbed behind a HAL. **Not a gate; documented as a
   non-radio smoke test.**
5. **Concurrency gate (real hardware):** the make-or-break above.

## Flash & debug

- **Bootloader:** flash a SoftDevice-aware USB-DFU bootloader once (Nordic
  secure bootloader from the SDK, or the Adafruit UF2 bootloader). The
  workspace has `build/bootloader_usb_s340.hex` and `dfu-keys/`.
- **App + SoftDevice:** packaged as a signed DFU `.zip` (adafruit-nrfutil /
  nrfutil), flashed over USB-C. `make dfu` builds it; `make flash-dfu` sends it.
- **Logs:** USB-CDC serial + OLED. SWD/`nrfjprog` + RTT supported as an optional
  deep-bring-up path but never required.

## Licensing / secrets

- ANT+ **network key** — free thisisant.com registration; kept in git-ignored
  `ant_network_key.h` (placeholder builds; real key needed for a Garmin to hear
  the footpod).
- ANT **evaluation license key** — passed by the Makefile (personal use).
- **DFU private key** — git-ignored under `dfu/`.
- S340 SoftDevice + API — licensed via thisisant.com, present in the workspace
  (`ANT_s340_nrf52_7.0.1/`); not re-committed.

## Milestones (implementation order)

1. **M0 — Repo + core + host tests green.** Scaffold v2 repo, vendor `core/`,
   port the host-test suite, everything green. No firmware yet.
2. **M1 — Firmware builds + boots.** Build system (Makefile, sdk_config,
   linker), `main.c` bring-up, USB-CDC log, DFU packaging. Boots on hardware and
   logs over USB-CDC. (Renode non-radio smoke here.)
3. **M2 — Test board.** OLED/LED/buzzer/button modules + `testboard.c` render;
   button injects a fake target into `workout_ctrl` and the screen shows the
   decision — the brain is observable with no radios.
4. **M3 — One radio at a time.** `ble_central` vs `mock_treadmill`;
   `ble_ctrl_svc` vs `mock_watch`; `ant_sdm` vs ANT stick. Each validated
   independently.
5. **M4 — Concurrency gate.** All three radios up; a `mock_watch` target-pace
   change drives a `mock_treadmill` belt change while ANT broadcasts. Then real
   treadmill + real watch.

## Out of scope (this cycle)

- The Connect IQ data field rewrite (keep the 15-byte `A6ED0004` contract).
- The ESP32 boards / old repo (untouched).
- Incline refinements beyond parity.
- A permanent on-device control UI (test aid only; designed so a fuller UI can
  drop in behind `testboard`/`app_state` later).
