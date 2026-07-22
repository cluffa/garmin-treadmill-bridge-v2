# nRF52840 Treadmill Bridge Rewrite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a clean, standalone nRF52840-only treadmill bridge where the device owns all belt-control policy, with an expansion-board test aid and a host BLE emulation harness so the whole system is testable with no Garmin watch and no treadmill.

**Architecture:** A platform-agnostic `core/` (vendored, host-tested protocol code — parsers, iFit FSM, workout policy, ANT SDM encoder) feeds/served by an nRF5-SDK-17.1.0 + S340 `firmware/` layer running three concurrent radios (BLE central → treadmill, BLE peripheral ← watch telemetry, ANT master → footpod). All belt decisions live in `core/workout_ctrl`. A compile-time-optional `testboard/` renders app state to an OLED and drives status LED/buzzer/button. Host mocks (fake watch + fake treadmill) exercise the real firmware over real BLE.

**Tech Stack:** C (C11 host, nRF5 SDK/GCC firmware), nRF5 SDK 17.1.0, S340 SoftDevice v7.0.1, ANT+ SDM profile, SSD1306 I²C OLED, USB-CDC ACM, Python (`bleak`) host mocks, Make, `nrfutil` DFU.

## Global Constraints

- **Repo:** `garmin-treadmill-bridge-v2/` — standalone, nRF52840-only. No ESP/NimBLE code.
- **`core/` purity:** every file under `core/` compiles for the host with **no** SoftDevice/nRF includes. nRF glue lives only in `firmware/`.
- **SDK:** nRF5 SDK 17.1.0 at `$(SDK_ROOT)` (default `$HOME/nRF5_SDK_17.1.0_ddde560`).
- **SoftDevice:** S340 v7.0.1. API headers: `S340_API` = `/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include`. Hex: `/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex`.
- **ARM toolchain:** `arm-none-eabi-gcc` (`/opt/homebrew/bin`), GNU_VERSION per `arm-none-eabi-gcc -dumpversion`.
- **Control contract (unchanged):** service `A6ED0001-2E7A-4E1D-9E3B-000000000000`-style base; char `A6ED0002` write (uppercase ctrl grammar), `A6ED0003` notify (compact `D`/`E`/`S` frames), `A6ED0004` write (raw 15-byte little-endian workout frame → `workout_ctrl`). CIQ MTU is 23; notify payload ≤ 20 bytes.
- **ANT:** device type 124 (Stride SDM), network key in git-ignored `ant_network_key.h` (placeholder builds; real key from thisisant.com). ANT eval license key passed by Makefile.
- **One-connection-at-a-time invariant:** the BLE central holds at most one treadmill link; connecting tears down the other protocol first and suppresses auto-reconnect while the other is connecting/connected. Never reintroduce simultaneous FTMS+iFit connections.
- **Secrets never committed:** `ant_network_key.h`, `dfu/dfu_private_key.pem`. Provide `.example` placeholders.
- **Source of truth for vendored files:** the old repo at `/Users/alex/workspace/nrf52/garmin-treadmill-bridge/` (referred to below as `$OLD`).
- **Commit style:** frequent, conventional commits; end bodies with the Co-Authored-By trailer.

---

## Milestone M0 — Repo scaffold + vendored `core/` + host tests green

### Task 0.1: Repo scaffold + build metadata

**Files:**
- Create: `.gitignore`, `Makefile` (top-level dispatch), `CLAUDE.md`, `README.md` (stub), `core/`, `firmware/`, `test/host/`, `test/mock/`, `test/renode/`, `dfu/` dirs.
- Reference: `$OLD/.gitignore`

**Interfaces:**
- Produces: top-level `make host-test`, `make firmware`, `make dfu` dispatch targets (bodies filled by later tasks; stub to `@echo TODO` + exit 0 only where the sub-Makefile doesn't exist yet).

- [ ] **Step 1:** Append to `.gitignore`: `ant_network_key.h`, `dfu/dfu_private_key.pem`, `firmware/_build/`, `test/host/*.o`, `test/host/test_*` (binaries), `*.zip`, `.DS_Store`.
- [ ] **Step 2:** Write `CLAUDE.md` with the build/flash commands, the `core/` purity invariant, the one-connection invariant, and the control contract (copy the relevant bullets from the spec verbatim).
- [ ] **Step 3:** Commit: `chore: scaffold v2 repo layout and build dispatch`.

### Task 0.2: Vendor `core/` protocol files

**Files (copy from `$OLD`, then fix includes to be self-contained under `core/`):**
- `core/model.h` ← `$OLD/components/bridge_core/model.h`
- `core/ftms_parse.{c,h}` ← same names
- `core/ifit_parse.{c,h}` ← same names
- `core/ifit_fsm.{c,h}` ← `$OLD/boards/xiao-nrf52840/ifit_poll.{c,h}` (rename `ifit_poll`→`ifit_fsm` symbols/guards; this is the host-tested pure FSM)
- `core/ctrl_dispatch.{c,h}`, `core/ctrl_frames.{c,h}`
- `core/ftms_devlist.{c,h}`, `core/connect_policy.{c,h}`
- `core/workout_ctrl.{c,h}` ← `$OLD/components/bridge_core/workout_ctrl.{c,h}`
- `core/ant_sdm_encode.{c,h}` ← `$OLD/components/bridge_core/ant_sdm_encode.{c,h}`

**Interfaces:**
- Produces (verbatim from the vendored headers — implementers of later tasks consume these): `ftms_parse()`, `ifit_parse()`, the `ifit_fsm` init/keepalive/frame-builder API, `ctrl_dispatch(const char* line, ...)`, `ctrl_frames` D/E/S builders, `ftms_devlist_*`, `connect_policy_*`, `workout_ctrl_on_frame()`, `workout_ctrl_tick()`, `ant_sdm_encode_page(const treadmill_state_t*, uint8_t page, uint8_t out[8])`.

- [ ] **Step 1:** Copy each file. Do **not** edit protocol logic. Only adjust `#include` paths so `core/` is self-contained (e.g. `#include "model.h"` stays; remove any `bridge_core/` prefixes).
- [ ] **Step 2:** Grep the copied `core/` for forbidden includes: `grep -rEl 'nrf|softdevice|ble_|host/ble|nimble|esp_' core/ --include=*.c --include=*.h` → **must be empty** (except protocol constant names like `ble_` only if they are pure macros — verify none pull nRF headers).
- [ ] **Step 3:** Rename `ifit_poll`→`ifit_fsm` throughout the copied FSM files and update its include guard.
- [ ] **Step 4:** Commit: `feat(core): vendor host-tested protocol layer from old repo`.

### Task 0.3: Port + run the host test suite

**Files:**
- Create: `test/host/Makefile` (adapt `$OLD/test/host/Makefile` to the new `core/` paths)
- Copy tests: `test_ftms_parse.c`, `test_ifit_parse.c`, `test_ifit_poll.c` (→ `test_ifit_fsm.c`), `test_ctrl_dispatch.c`, `test_ctrl_frames.c`, `test_ftms_devlist.c`, `test_connect_policy.c`, `test_workout_ctrl.c`, `test_ant_sdm_encode.c` ← `$OLD/test/host/`

**Interfaces:**
- Consumes: all `core/` symbols from Task 0.2.
- Produces: `make -C test/host` builds and runs every `test_*`; exit 0.

- [ ] **Step 1:** Copy the test `.c` files and Makefile; update include/vpath to `../../core`. Rename the ifit_poll test to `test_ifit_fsm.c` and its symbols.
- [ ] **Step 2:** Run `make -C test/host` and verify it **fails** first if any path is wrong (expected: compile error naming the missing path). Fix paths.
- [ ] **Step 3:** Run `make -C test/host` again. Expected: every test prints PASS and the target exits 0.
- [ ] **Step 4:** Wire top-level `make host-test` → `make -C test/host`.
- [ ] **Step 5:** Commit: `test(core): port host test suite; all green against v2 core`.

**M0 exit gate:** `make host-test` green. No firmware yet.

---

## Milestone M1 — Firmware builds, boots, logs over USB-CDC

### Task 1.1: SDK config, linker, and Makefile (build skeleton, empty `main`)

**Files:**
- Create: `firmware/sdk_config.h` ← copy `$OLD/boards/xiao-nrf52840/sdk_config.h` (unmodified SDK template; project overrides go in `app_config.h`).
- Create: `firmware/app_config.h` ← copy `$OLD/boards/xiao-nrf52840/app_config.h`, then trim to only what a bare boot needs (keep `USE_APP_CONFIG`, log, clock, power; leave BLE/ANT counts as they are).
- Create: `firmware/xiao_nrf52840_s340.ld` ← copy `$OLD/boards/xiao-nrf52840/xiao_nrf52840_s340.ld`.
- Create: `firmware/board_pins.h` (XIAO nRF52840 + expansion board pin map — see Task 2.x for the peripheral pins; here define at least LED_R/G/B P0.26/P0.30/P0.06 active-low, and reserve I2C SDA/SCL, buzzer, button).
- Create: `firmware/Makefile` ← adapt `$OLD/boards/xiao-nrf52840/Makefile`: set `SRC` to the new `firmware/*.c` + `core/*.c` + SDK sources; `S340_API` default to the workspace path; add `dfu`, `flash-dfu`, `flash-sd`, `flash-app` targets.
- Create: `firmware/main.c` — minimal: SoftDevice enable (S340), `app_timer` init, `nrf_pwr_mgmt`, `NRF_LOG` init, an idle loop logging a heartbeat.

**Interfaces:**
- Produces: `make -C firmware` yields `_build/nrf52840_xxaa.hex`.

- [ ] **Step 1:** Copy the SDK config, app_config, linker. Verify FLASH origin (S340 `APP_CODE_BASE`) and RAM origin in the linker match S340 v7.0.1 (the app will log the required RAM start over the log backend on mismatch — note this for M1 boot).
- [ ] **Step 2:** Write minimal `main.c`: `nrf_sdh_enable_request()` with S340, `nrf_sdh_ble` not yet, `app_timer_init()`, `NRF_LOG_INIT`, loop `NRF_LOG_INFO("alive %u", cnt++)` + `nrf_pwr_mgmt_run()`.
- [ ] **Step 3:** `make -C firmware GNU_INSTALL_ROOT=/opt/homebrew/bin/ GNU_VERSION=$(arm-none-eabi-gcc -dumpversion) S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include` — expected: links, produces `.hex`. Fix missing SDK source paths until it links.
- [ ] **Step 4:** Wire top-level `make firmware`.
- [ ] **Step 5:** Commit: `feat(fw): S340 build skeleton links; minimal boot main`.

### Task 1.2: USB-CDC logging + interactive ctrl console

**Files:**
- Create: `firmware/usb_cdc_log.{c,h}` — `app_usbd` + `app_usbd_cdc_acm`; register as an `NRF_LOG` backend (or fallback: format lines and write to CDC directly). Provide a line reader: on each received line, call `ctrl_dispatch(line, &sink)` where `sink` writes replies back to CDC.
- Modify: `firmware/main.c` — init usb_cdc_log; route heartbeat there.
- Modify: `firmware/app_config.h` — enable `APP_USBD_ENABLED`, `APP_USBD_CDC_ACM_ENABLED`, USB descriptors.

**Interfaces:**
- Consumes: `ctrl_dispatch()` (core).
- Produces: `usb_cdc_log_init(void)`, `usb_cdc_log_write(const char*)`, and a weak `usb_cdc_on_line(const char*)` hook.

- [ ] **Step 1:** Implement `usb_cdc_log.c`; wire USBD event handlers and CDC RX ring buffer → line assembly.
- [ ] **Step 2:** Build (`make -C firmware …`). Expected: links.
- [ ] **Step 3:** (host, no HW) run the Renode non-radio smoke (Task 1.3) to confirm boot path; on HW: enumerate as USB-CDC, `screen /dev/cu.usbmodem*` shows the heartbeat, typing `STATUS` returns a reply.
- [ ] **Step 4:** Commit: `feat(fw): USB-CDC log backend + ctrl console (probe-free debug)`.

### Task 1.3: Renode non-radio boot smoke (bounded spike)

**Files:**
- Create: `test/renode/nrf52840_boot.resc` (load platform `@platforms/cpus/nrf52840.repl`, `sysbus LoadELF @../../firmware/_build/nrf52840_xxaa.out`, start, assert log shows heartbeat).
- Create: `test/renode/README.md` documenting: **S340 radio is NOT emulated** (TEMP peripheral + RSSI/errata gaps); this only proves the CPU boots and drives non-radio peripherals. SoftDevice calls must be behind a HAL or stubbed for this to run.

**Interfaces:**
- Produces: `renode --console -e "include @test/renode/nrf52840_boot.resc; …"` prints the heartbeat and exits.

- [ ] **Step 1:** Write the `.resc`; if `renode` is not installed, the README documents the manual invocation and the task is marked non-gating.
- [ ] **Step 2:** Run if available; capture output. Expected: heartbeat lines appear (SoftDevice-dependent init stubbed).
- [ ] **Step 3:** Commit: `test(renode): non-radio boot smoke + honest limitations doc`.

**M1 exit gate:** firmware links to a `.hex`; on hardware it boots and logs over USB-CDC and answers `STATUS` on the console. (DFU packaging in Task 4.x.)

---

## Milestone M2 — Expansion-board test aid (the brain, observable, no radios)

### Task 2.1: `app_state` — single source of truth

**Files:**
- Create: `firmware/app_state.{c,h}`

**Interfaces:**
- Produces:
  ```c
  typedef enum { LINK_DOWN, LINK_SCANNING, LINK_CONNECTING, LINK_UP } link_state_t;
  typedef struct {
    treadmill_state_t treadmill;      // latest parsed belt state
    link_state_t      central_link;   // treadmill BLE central
    char              treadmill_name[24];
    bool              watch_connected; // BLE peripheral (ctrl svc) link
    bool              ant_broadcasting;
    float             resolved_target_mps; // last target workout_ctrl commanded
    uint32_t          last_fault_code;     // 0 = none
  } app_state_t;
  app_state_t* app_state(void);           // singleton
  void app_state_init(void);
  void app_state_set_fault(uint32_t code);
  ```
- Consumes: `treadmill_state_t` (core/model.h).

- [ ] **Step 1:** Write header + `.c` (static singleton, zeroed on init).
- [ ] **Step 2:** Add a host unit test `test/host/test_app_state.c`? — **No** (app_state is firmware-only glue). Instead verify it compiles into the firmware build.
- [ ] **Step 3:** Build firmware. Expected: links. Commit: `feat(fw): app_state single source of truth`.

### Task 2.2: HW peripheral drivers — LED, buzzer, button

**Files:**
- Create: `firmware/testboard/hw_led.{c,h}` — onboard RGB LED (P0.26/P0.30/P0.06, active-low). API: `hw_led_set(bool r,bool g,bool b)`, `hw_led_status(link_state_t central, bool watch, bool fault)`.
- Create: `firmware/testboard/hw_buzzer.{c,h}` — PWM tone. API: `hw_buzzer_init()`, `hw_buzzer_chirp(uint16_t freq_hz, uint16_t ms)`.
- Create: `firmware/testboard/hw_button.{c,h}` — GPIOTE + `app_button` debounce. API: `hw_button_init(void(*on_short)(void), void(*on_long)(void))`.
- Confirm expansion-board pins in `board_pins.h` (Seeed XIAO expansion base: user button on the XIAO D1/P0.03; passive buzzer on A3/D3; OLED I²C on D4/D5 = P0.04/P0.05). **Verify against the actual board silkscreen before finalizing; document the source.**

**Interfaces:**
- Consumes: `link_state_t` (app_state.h).
- Produces: the three `hw_*` APIs above.

- [ ] **Step 1:** Implement `hw_led.c` (GPIO out, active-low). Build.
- [ ] **Step 2:** Implement `hw_buzzer.c` (`nrf_drv_pwm` or `app_pwm`). Build.
- [ ] **Step 3:** Implement `hw_button.c` (`app_button`, 50 ms debounce; long-press ≥ 800 ms via `app_timer`). Build.
- [ ] **Step 4:** Commit: `feat(testboard): LED/buzzer/button drivers`.

### Task 2.3: SSD1306 OLED driver

**Files:**
- Create: `firmware/testboard/ssd1306.{c,h}` — `nrf_drv_twi` I²C, 128×64, addr 0x3C. API: `ssd1306_init()`, `ssd1306_clear()`, `ssd1306_text(uint8_t col,uint8_t row,const char*)`, `ssd1306_show()`. Include a 5×7 font table.

**Interfaces:**
- Produces: the `ssd1306_*` API.

- [ ] **Step 1:** Implement TWI init + SSD1306 init sequence + a framebuffer + 5×7 font + text render. Build.
- [ ] **Step 2:** On HW: `ssd1306_text(0,0,"HELLO"); ssd1306_show();` shows text. (No HW → visual check deferred; ensure it links.)
- [ ] **Step 3:** Commit: `feat(testboard): SSD1306 128x64 I2C OLED driver`.

### Task 2.4: `testboard` render + test-action injection

**Files:**
- Create: `firmware/testboard/testboard.{c,h}` — behind `#if TESTBOARD`. Renders `app_state()` to the OLED (link status line, belt speed, resolved target, workout step, fault); drives `hw_led_status` + buzzer on transitions; button short-press cycles a test action: `SCAN → CONNECT-NEXT → INJECT test target (e.g. build a synthetic 15-byte A6ED0004 frame for 8.0 km/h and call workout_ctrl_on_frame) → STOP`. Long-press = system reset.
- Modify: `firmware/main.c` — `#if TESTBOARD` init + a ~5 Hz `app_timer` render tick.
- Modify: `firmware/Makefile` — `TESTBOARD ?= 1`; pass `-DTESTBOARD=$(TESTBOARD)`.

**Interfaces:**
- Consumes: `app_state()`, `hw_*`, `ssd1306_*`, `workout_ctrl_on_frame()`.
- Produces: `testboard_init()`, `testboard_render_tick()`, `testboard_on_button_short/long()`.

- [ ] **Step 1:** Implement the render (compose lines from app_state) + the button action cycle + the synthetic-frame injector (document the 15-byte layout inline, sourced from `core/workout_ctrl.h`).
- [ ] **Step 2:** Build with `TESTBOARD=1` and `TESTBOARD=0`. Expected: both link; `=0` compiles the module to no-ops.
- [ ] **Step 3:** On HW (no radios yet): press button → OLED shows the injected target and `workout_ctrl`'s resolved decision; LED reflects state. **This proves the brain is observable with no watch/treadmill.**
- [ ] **Step 4:** Commit: `feat(testboard): render app_state + button-driven brain self-test`.

**M2 exit gate:** with `TESTBOARD=1`, pressing the button injects a fake target and the OLED shows `workout_ctrl`'s decision — no radios involved.

---

## Milestone M3 — Radios, one at a time (validated against host mocks)

### Task 3.1: `ble_ctrl_svc` — GATT server (watch control side) + `mock_watch.py`

**Files:**
- Create: `firmware/ble_ctrl_svc.{c,h}` — rebuild fresh using `$OLD/boards/xiao-nrf52840/platform_ble_ctrl_svc.c` as reference (validate every handler). S340 GATT server: A6ED service, chars `0002`/`0003`/`0004`; advertising with A6ED UUID in scan response; `0002`→`ctrl_dispatch`, `0004`→`workout_ctrl_on_frame`; `0003` notify D/E/S. Update `app_state()->watch_connected`.
- Create: `test/mock/mock_watch.py` — self-contained `uv` `bleak` script: scan for the A6ED service, connect, write a synthetic 15-byte workout frame (target pace configurable), read notifications. Adapt from `$OLD/test/mock/mock_ctrl_watch.py`.
- Modify: `firmware/main.c` — init BLE stack (`nrf_sdh_ble`) + ble_ctrl_svc.

**Interfaces:**
- Consumes: `ctrl_dispatch()`, `workout_ctrl_on_frame()`, `ctrl_frames` builders, `app_state`.
- Produces: `ble_ctrl_svc_init()`, `ble_ctrl_svc_notify(const uint8_t* frame, uint16_t len)`, `ble_ctrl_svc_advertise_start()`.

- [ ] **Step 1:** Implement the GATT server + advertising. Build (BLE only; ANT off for now).
- [ ] **Step 2:** Write `mock_watch.py`.
- [ ] **Step 3:** On HW: flash; run `mock_watch.py --target 8.0`; confirm over USB-CDC log that `workout_ctrl` received the frame and resolved 8.0 km/h; OLED shows it. Expected: brain reacts to the fake watch.
- [ ] **Step 4:** Commit: `feat(fw): S340 GATT control service + mock_watch harness`.

### Task 3.2: `ble_central` — treadmill link + `mock_treadmill.py`

**Files:**
- Create: `firmware/ble_central.{c,h}` — rebuild fresh using `$OLD/boards/xiao-nrf52840/platform_ble_central.c` (783 lines) as reference; validate line-by-line. S340 GATT client: scan FTMS `0x1826` / iFit `0x1533`, build `ftms_devlist`, connect per `connect_policy`, discover + subscribe, pump notifications → `ftms_parse`/`ifit_parse` → `app_state`. Own control writes; drive iFit writes at the keepalive phase via `ifit_fsm` off S340 events. Enforce the one-connection invariant.
- Create: `test/mock/mock_treadmill.py` — FTMS (and optional iFit) peripheral; accepts control-point speed/incline writes; notifies speed. Adapt from `$OLD/test/mock/mock_treadmill.py`.
- Modify: `firmware/main.c` — init ble_central; wire scan/connect to the console + button.

**Interfaces:**
- Consumes: `ftms_parse`, `ifit_parse`, `ifit_fsm`, `ftms_devlist`, `connect_policy`, `app_state`.
- Produces: `ble_central_init()`, `ble_central_scan_start()`, `ble_central_connect(idx)`, `ble_central_set_speed(float mps)`, `ble_central_disconnect()`.

- [ ] **Step 1:** Implement scan + devlist + connect policy + discovery + subscribe (FTMS first). Build.
- [ ] **Step 2:** Add iFit path + keepalive-phased writes via `ifit_fsm`. Build.
- [ ] **Step 3:** Write `mock_treadmill.py`.
- [ ] **Step 4:** On HW: run `mock_treadmill.py`; firmware scans, connects, parses speed (OLED/log), and a `SPEED 8.0` console command lands a control-point write the mock prints. Expected: forward parse + reverse control both work against the mock.
- [ ] **Step 5:** Commit: `feat(fw): S340 BLE central treadmill link + mock_treadmill harness`.

### Task 3.3: `ant_sdm` — ANT master footpod

**Files:**
- Create: `firmware/ant_sdm.{c,h}` — rebuild using `$OLD/boards/xiao-nrf52840/platform_ant_sdm.c` as reference. Open an ANT master channel (device type 124, SDM period 8192), broadcast pages from `ant_sdm_encode_page(app_state()->treadmill, page, buf)` on the ANT TX event. Use `ant_network_key.h`.
- Modify: `firmware/main.c` — enable `nrf_sdh_ant`; init ant_sdm; set `app_state()->ant_broadcasting`.

**Interfaces:**
- Consumes: `ant_sdm_encode_page()`, `app_state`.
- Produces: `ant_sdm_init()`, `ant_sdm_start()`, `ant_sdm_on_tx_event()`.

- [ ] **Step 1:** Implement the ANT master channel + page rotation. Build (BLE central + ANT together now).
- [ ] **Step 2:** On HW: with the ANT USB stick (`ANT-SDK_Mac.3.5` tools) or a second Garmin, confirm a discoverable SDM footpod reporting the belt speed. Expected: valid footpod.
- [ ] **Step 3:** Commit: `feat(fw): ANT+ SDM master footpod broadcast`.

**M3 exit gate:** each radio validated independently against its mock/stick.

---

## Milestone M4 — Concurrency gate + DFU packaging (hardware)

### Task 4.1: DFU packaging + flash workflow

**Files:**
- Create: `dfu/README.md` — one-time bootloader flash (`build/bootloader_usb_s340.hex` or Nordic secure bootloader), key handling, everyday `make dfu`/`make flash-dfu`.
- Create: `dfu/dfu_public_key.c` (from workspace `dfu-keys/`), `dfu/dfu_private_key.pem.example`.
- Modify: `firmware/Makefile` — `dfu:` builds a signed zip (`nrfutil pkg generate --hw-version 52 --sd-req <S340_ID> --application _build/nrf52840_xxaa.hex --key-file ../dfu/dfu_private_key.pem app.zip`); `flash-dfu:` sends over USB-CDC DFU; `flash-sd`/`flash-app` for SWD fallback.

**Interfaces:**
- Produces: `make -C firmware dfu` → `app.zip`; `make -C firmware flash-dfu SERIAL=/dev/cu.usbmodemXXXX`.

- [ ] **Step 1:** Determine the S340 `--sd-req` FWID; wire the `dfu` target. Run `make -C firmware dfu`. Expected: `app.zip` produced.
- [ ] **Step 2:** Document + wire `flash-dfu`. (HW: performs the update.)
- [ ] **Step 3:** Commit: `feat(dfu): signed USB-DFU packaging + flash targets`.

### Task 4.2: Three-radio concurrency gate (real hardware)

**Files:**
- Create: `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md` — record the run.

- [ ] **Step 1:** All three radios up: run `mock_treadmill.py` + `mock_watch.py` simultaneously; ANT broadcasting. Change `mock_watch --target`; confirm the belt-speed control write lands on `mock_treadmill` **while** ANT broadcasts and the ctrl link is connected.
- [ ] **Step 2:** Repeat against the **real** treadmill + **real** Garmin watch (SDM sensor paired, workout target driving the belt via the device brain).
- [ ] **Step 3:** Record pass/fail + timing observations. Commit: `docs: concurrency gate results`.

**M4 exit gate (project done):** target-pace change on the watch drives the belt through the device while the watch simultaneously reads speed over the ANT footpod.

---

## README + finalize

### Task 5.1: README + CLAUDE.md finalize

- [ ] **Step 1:** Write `README.md`: what it is, the device-is-the-brain topology diagram, hardware (XIAO nRF52840 + expansion board), build/flash, the host mock harness, the testing layers, "built with AI assistance" note, licensing/secrets note. No secrets/real keys.
- [ ] **Step 2:** Verify `make host-test` green and `make firmware` links in a clean checkout.
- [ ] **Step 3:** Commit: `docs: README + finalize build/testing docs`.

---

## Self-review notes (coverage map)

- Spec "device is the brain" → Tasks 0.2 (workout_ctrl vendored), 2.4/3.1 (frame → workout_ctrl), 3.2 (control writes).
- Spec "reuse proven bytes, rebuild glue" → M0 (vendor core), M3 (rebuild ble_central/ctrl_svc/ant_sdm fresh from reference).
- Spec "expansion board test aid, optional" → M2 (all `#if TESTBOARD`, no-op at `=0`).
- Spec "emulate watch + treadmill" → `mock_watch.py` (3.1), `mock_treadmill.py` (3.2).
- Spec "Renode bounded, non-radio" → Task 1.3 with honest limitations doc.
- Spec "USB-DFU + USB-CDC, no probe" → Tasks 1.2 (CDC), 4.1 (DFU).
- Spec "concurrency make-or-break gate" → Task 4.2 (real HW).
- Spec "host tests green gate" → M0 exit gate.
