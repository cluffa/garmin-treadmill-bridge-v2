# Hardware Test Plan — bring-up ordered for debugging

Goal: take the board from "just flashed" to "all three radios working with a real
treadmill and watch," in the order that makes failures easy to localize. Each
phase validates one layer and depends only on the layers below it, so a failure
points at the layer you just added — not a tangle of interacting unknowns.

Guiding rules:
- **One variable at a time.** Add exactly one new thing per step.
- **Mocks before real hardware.** Mocks are deterministic and controllable; a
  real treadmill/watch adds RF, pairing, and vendor quirks. Prove the firmware
  against mocks first, then swap in the real peer.
- **Drive from the USB console first.** The console is both your log output *and*
  a full control input (`ctrl_dispatch` runs on every console line), so you can
  exercise scan/connect/speed/stop and the ANT path **without a watch at all**.
  That isolates the treadmill + ANT paths from the watch path.
- **Simplest radio first.** ANT (one-way broadcast, no handshake) → BLE
  peripheral (you are the server) → BLE central (you are the client: scan,
  connect, discover, subscribe, parse, control — the most state).

Phases 6–7 **are** the acceptance gate in
`docs/superpowers/test-logs/2026-07-21-concurrency-gate.md` — record final
PASS/FAIL results there; this document is the road that gets you to a green gate.

---

## Instruments — how you observe the board

| Instrument | Shows | Notes |
|---|---|---|
| **USB-CDC console** (primary) | `alive N` heartbeat + all ctrl JSON replies | `screen /dev/cu.usbmodem<CHIPSERIAL>1 115200`. Also your control input. Port-mapping gotcha: `docs/flashing.md` §7. |
| **Mock terminals** | what the *peer* sees | `mock_treadmill.py` prints CP writes; `mock_watch.py` prints decoded D/E/S frames. |
| **OLED** (if `TESTBOARD=1`) | `app_state` render (status, speed, links) | Visual, no host needed. |
| **`nrf_log` RTT + USB-CDC** (always on) | detailed SDK/event logs (`NRF_LOG_INFO`) | Backend = **RTT** (available over SWD probe) + **USB-CDC** (the same serial console you use for ctrl commands). `screen /dev/cu.usbmodem<CHIPSERIAL>1 115200` shows both logs and ctrl JSON — this is the everyday debug channel. RTT reads require a debug probe (`pyocd commander` or `JLinkRTTClient`); halting for RTT briefly breaks USB enumeration, so peek quickly. |
| **pyocd RAM peek** | heartbeat counter, fault registers, when hung | `pyocd commander -t nrf52840 -c halt -c "read32 <addr>" -c go`. Halting breaks live USB enumeration — peek quickly, then `go`. |
| **ANT+ receiver** | footpod (device type 124) | A Garmin watch "add sensor," an ANT+ USB stick (`openant`), or an Android ANT+ sampler app. |

**Don't gate pass/fail on `nrf_log`** — infer state from `STATUS`/`LIST` JSON and
the mock terminals, which are always visible over USB.

### Console command reference (send uppercase, newline-terminated)

| Command | Reply (USB console = JSON) |
|---|---|
| `STATUS` | `{"cmd":"status","connected":false}` / `…,"connected":true,"name":"…"}` |
| `LIST` | `{"cmd":"list","devices":[{…index,name,proto:FTMS\|iFit,rssi…}]}` |
| `SCAN` | `{"cmd":"scan","ok":true}` |
| `CONNECT <n>` | `{"cmd":"connect","ok":true}` / `…,"ok":false,"err":"bad index"}` |
| `SPEED <kmh>` | `{"cmd":"speed","ok":true\|false}` |
| `INCLINE <pct>` | `{"cmd":"incline","ok":true\|false}` |
| `STOP` | `{"cmd":"stop","ok":true\|false}` |
| (anything else) | `{"event":"error","msg":"unknown command"}` |

> Over BLE (the watch path) the *notify* channel `A6ED0003` speaks compact
> `D`/`E`/`S` frames, **not** JSON. JSON is the USB-console rendering only.

---

## Bill of materials

- XIAO nRF52840 (provisioned per `docs/flashing.md` §4) + Pico CMSIS-DAP probe.
- Two USB-C cables (known-good **data** cables — see the cable gotcha in flashing §8).
- Host mocks: `test/mock/mock_treadmill.py`, `test/mock/mock_watch.py` (run with `uv`).
- A BLE scanner app (nRF Connect, iOS/Android/desktop) — handy in Phase 4.
- An ANT+ receiver for Phase 2 (see Instruments table).
- Phase 7 only: the real treadmill (FTMS or iFit) + the real Garmin watch with the CIQ field.

---

## Phase 0 — Pre-flight & boot (no radios peered)

**Goal:** the logic is already proven off-device; confirm the board boots clean
and you can see it.

1. [ ] `make host-test` → **9/9 OK** (core logic regression before touching hardware).
2. [ ] Build + provision per `docs/flashing.md` (`make -C firmware flash-full`).
3. [ ] Attach the USB console. Confirm a steady `alive N` heartbeat (N incrementing).
4. [ ] If `TESTBOARD=1`: OLED shows the idle/status screen; blue LED behaves.
5. [ ] No boot loop, no stall (heartbeat keeps counting for 30 s).

**PASS:** heartbeat counts continuously; console is responsive.

**If it fails:**
- No heartbeat / no port → boot hang or USB enum. Confirm the correct port
  (flashing §7), try a known-good cable/port (flashing §8). Peek the heartbeat
  counter over SWD to tell "app hung" (counter frozen) from "USB not enumerating"
  (counter advancing but no port).
- Heartbeat but board resets → likely an early `APP_ERROR_CHECK`. Halt over SWD
  and read the fault; suspect LFCLK (should be RC — `sdk_config.h`) or
  power-init order (`main.c`).

---

## Phase 1 — Console control plane (no peer radios)

**Goal:** prove USB-CDC RX/TX, the line reader, `ctrl_dispatch`, and the
`machine` facade wiring — with **nothing to connect to yet**.

1. [ ] `STATUS` → `{"cmd":"status","connected":false}`.
2. [ ] `LIST` → `{"cmd":"list","devices":[]}` (empty; nothing scanned yet).
3. [ ] `SCAN` → `{"cmd":"scan","ok":true}`.
4. [ ] A short single command (`STATUS`, 7 B) replies **immediately** (regression
   guard for the `read_any` RX fix — a fixed-length read would stall until 64 B).
5. [ ] Garbage line (`FOO`) → `{"event":"error","msg":"unknown command"}`.

**PASS:** every command returns well-formed JSON promptly.

**If it fails:**
- Nothing echoes back but heartbeat streams → RX path. This is exactly the bug
  fixed in `usb_cdc_log.c` (must be `app_usbd_cdc_acm_read_any`); verify DTR is
  set by your terminal (`line_state` DTR bit at `m_cdc_acm_data + 0x14`).
- Replies are malformed → `ctrl_dispatch` / `snprintf` buffer issue (`core/`).
  This is host-testable: `test/host/test_ctrl_dispatch`.

---

## Phase 2 — ANT master (simplest radio: one-way broadcast)

**Goal:** validate the ANT stack, license key, network key, and timeslot in
isolation. No connection handshake, so this is the cleanest radio to bring up.

1. [ ] Have an ANT+ receiver ready (Garmin "add sensor," ANT+ stick, or phone app).
2. [ ] Boot the bridge (ANT starts broadcasting on its own).
3. [ ] The receiver discovers a **footpod / Stride sensor, device type 124**,
   device number = lower 16 bits of the chip ID.
4. [ ] Pace/cadence pages update at **~4 Hz** (channel period 8192 → 32768/8192).

**PASS:** an ANT+ receiver sees the footpod and its pages update.

**If it fails:**
- No ANT device at all → `sd_ant_enable()` / channel-open failure. **Most likely
  the ANT eval license key** (`ant_license.mk`) or the network key
  (`ant_network_key.h`) is missing/placeholder — the build warns when the key is
  empty. Confirm both exist with real values.
- Device seen but no page updates → the tx-event encode path
  (`core/ant_sdm_encode.c`, host-tested by `test_ant_sdm_encode`) or the tx
  cadence in `firmware/ant_sdm.c`.

---

## Phase 3 — BLE central (treadmill link), driven from the console

**Goal:** the most complex radio — scan → connect → service discovery →
subscribe → parse → control-point write — validated against the **mock
treadmill**, driven entirely from the USB console (no watch involved).

1. [ ] Host: `cd test/mock && uv run mock_treadmill.py` (advertises FTMS 0x1826,
   prints `tx speed=…`).
2. [ ] Console `SCAN`, then `LIST` → the mock appears in `devices[]` with a name,
   `proto:"FTMS"`, and an RSSI.
3. [ ] Console `CONNECT 0` → `{"cmd":"connect","ok":true}`.
4. [ ] Console `STATUS` → `connected:true` with the mock's `name`.
5. [ ] Console `SPEED 8.0` → `{"cmd":"speed","ok":true}` **and** the mock
   treadmill terminal prints a CP `set speed 8.00 km/h`.
6. [ ] `SPEED 10.5` updates the mock; `STOP` → mock sees stop.

**PASS:** connect, live `STATUS`, and speed/stop all round-trip to the mock.

**If it fails (localize by where the chain stops):**
- Mock not in `LIST` → scan/advertising filter or `adv_name` parse
  (`ble_central.c`); confirm the mock is actually advertising (nRF Connect).
- `LIST` shows it but `CONNECT` fails → connection or GATT discovery
  (subscribe to `0x2ACD`, find CP `0x2AD9`).
- Connected but `SPEED` doesn't move the mock → FTMS control-point encoding.
  Cross-check `test/host/test_ftms_parse` / `test_workout_ctrl`; the encoding is
  `core/`, so a host test likely reproduces it.
- iFit path (`0x1533`) is **not** exercisable with this mock — it needs a real
  iFit treadmill (e.g. NordicTrack). Defer iFit to Phase 7.

---

## Phase 4 — BLE peripheral (watch link), via mock_watch

**Goal:** validate the watch-facing side independently: advertising as
`TMILL-CTRL`, the `A6ED` ctrl service, ctrl-grammar writes, workout-telemetry
frames, and the `D`/`E`/`S` notify frames.

1. [ ] Host: `cd test/mock && uv run mock_watch.py --target 8.0`.
2. [ ] Mock connects to `TMILL-CTRL` and subscribes to `A6ED0003`
   (`connected: True`, `subscribed`).
3. [ ] From the mock, `STATUS` / `LIST` → sensible `D`/`E` frames decode.
4. [ ] The mock's workout frame to `A6ED0004` decodes (`workout_ctrl`) — the mock
   logs `=> wkt <hex>`; no parse error on-device.

**PASS:** the watch mock connects, subscribes, and exchanges frames without error.

**If it fails:**
- Won't discover `TMILL-CTRL` → advertising (`ble_ctrl_svc.c`); check with nRF Connect.
- Connects but notify frames are wrong → `D`/`E`/`S` framing in `ble_ctrl_svc.c`
  (compact frames must be ≤ 20 B for the CIQ 23-byte MTU) or `ctrl_frames`
  (`test/host/test_ctrl_frames`).
- Workout frame rejected → `workout_ctrl` decode (`test/host/test_workout_ctrl`).

---

## Phase 5 — Pairwise concurrency (isolate timeslot contention)

**Goal:** before all three, prove each *pair* coexists — so if the full set
faults you already know which pair is the culprit. S340 BLE+ANT timeslot
contention is the highest-risk interaction.

1. [ ] **Two BLE links:** `mock_watch.py` connected **and** (via the mock or the
   console) connect to `mock_treadmill.py`. Both links stay up ≥ 60 s.
2. [ ] **BLE + ANT:** with a BLE link up, the ANT+ receiver still sees the
   footpod updating. No stall, no disconnect, no fault.

**PASS:** each pair runs 60 s with no hard-fault, assert, disconnect, or console stall.

**If it fails:**
- Hard-fault when the second radio joins → timeslot / resource conflict. Halt
  over SWD, read the fault registers, note which radio's activity preceded it.
  This is the core risk the v2 architecture must clear; capture the exact repro
  before changing anything (systematic-debugging Phase 1).

---

## Phase 6 — All three radios, mocks (= Concurrency Gate, Part A)

Run **Part A** of `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`:
`mock_treadmill` + `mock_watch` + ANT all up, 60 s soak, one speed change, no
faults. **Record results in that checklist**, not here.

**PASS:** Gate checks A1–A5 all PASS.

---

## Phase 7 — Real world (= Concurrency Gate, Part B)

Run **Part B** of the gate: real Garmin watch (BLE peripheral + ANT+ footpod
readback) and real treadmill (FTMS *or* iFit central), full loop — watch sets
target → belt follows → footpod pace shows on the watch — with a 60 s continuous
run and a mid-run speed change. **Record in the gate checklist.**

Swap in one real peer at a time if anything misbehaves: real treadmill with the
mock watch first, then real watch with the mock treadmill, then both real. That
keeps the "real hardware" variable isolated the same way the earlier phases did.

**PASS:** Gate checks B1–B5 all PASS → the v2 device-is-the-brain gate is green.

---

## Phase 8 — Robustness & fault injection

**Goal:** confirm graceful recovery, not just the happy path.

1. [ ] **Treadmill drop:** power off the treadmill mid-connect → `STATUS` returns
   to `connected:false`; a re-`SCAN`/`CONNECT` recovers. No fault.
2. [ ] **SCAN mid-connect:** issue `SCAN` while a connect is in flight → it tears
   down and rescans cleanly (regression guard for the `ble_central.c` fix). No
   double-connect, no fault.
3. [ ] **Watch drop:** disconnect the watch/mock → the treadmill link and ANT
   keep running; watch can reconnect.
4. [ ] **One-connection invariant:** connecting a second treadmill tears down the
   first; never two simultaneous FTMS+iFit links.
5. [ ] **STOP safety:** `STOP` halts the belt from any state.
6. [ ] **USB re-enumeration:** unplug/replug the console → heartbeat + commands
   resume; radios unaffected.
7. [ ] **DFU round-trip:** `make dfu` → `make dfu-enter` → `make flash-dfu` →
   board reboots to the app and all links come back (dogfoods the update path).

**PASS:** every disturbance recovers without a hard-fault or a stuck state.

---

## When a phase fails — process

Follow `superpowers:systematic-debugging`: **find root cause before any fix.**
1. Capture the exact repro and the console/mock output at the point of failure.
2. Because this is a multi-layer system (host mock → RF → SoftDevice → app →
   `core/`), note *which layer* the evidence points to (the phase you were in
   already narrows it).
3. If the logic is in `core/`, reproduce it with a host test first — it's faster
   and deterministic than re-running on hardware.
4. One hypothesis, one minimal change, re-test. Don't stack fixes.
