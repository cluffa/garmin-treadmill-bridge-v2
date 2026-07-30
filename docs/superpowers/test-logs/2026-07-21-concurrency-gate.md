# Concurrency Gate -- 2026-07-21 (run 2026-07-29)

**Result: Part A PASS. Part B partial — B2/B3 PASS on a real iFit treadmill;
B1/B4/B5 still need the real Garmin watch.**

Run on 2026-07-29 by alex (hardware operator) with Claude Code reading device
state over SWD. Firmware: branch `fix/code-review-2026-07-27` at `835f588`
**plus four uncommitted files** — `firmware/app_config.h`,
`firmware/ble_central.c`, `firmware/ble_ctrl_svc.c`, `test/mock/mock_treadmill.py`.
Those four are what this run verifies; see "Fixes verified" below.

## Purpose

Prove that all three radios (BLE peripheral, BLE central, ANT master) operate
concurrently under the S340 SoftDevice without deadlocks, timeouts, or
hard-faults. This is the make-or-break gate for the v2 "device-is-the-brain"
architecture.

## How this run deviated from the plan

Two deliberate substitutions, both in the *harder* direction:

- **The treadmill peer was the REAL iFit machine, not `mock_treadmill.py`.**
  It advertises as `I_TL`. This is strictly stronger than the plan's Part A:
  the iFit path (`0x1533`) is explicitly *not* exercisable against the FTMS
  mock, so Part A's A2/A3 were previously deferred to Part B entirely.
  A2/A3 below are therefore recorded against real hardware.
- **The watch peer was `mock_watch.py`,** not the real Garmin. This is the
  "real treadmill + mock watch first" isolation step the hardware test plan
  recommends for Phase 7. B1/B4/B5 remain open because of it.

## Prerequisites

- [x] XIAO nRF52840 running the build under test. `UICR NRFFW[0] = 0x000F4000`
      (bootloader intact), `s_heartbeat_cnt` monotonic at 1 Hz.
- [x] S340 SoftDevice v7.0.1 — `BLE stack enabled`, `ANT stack enabled`.
- [ ] ~~USB-CDC console~~ **STILL UNAVAILABLE — USB enumeration is broken**
      (see `docs/HANDOFF.md`). All observation this run was over SWD:
      ```sh
      nm -S firmware/_build/nrf52840_xxaa.out | grep -E '_acUpBuffer|s_heartbeat_cnt| s_state'
      pyocd commander -t nrf52840 -O connect_mode=attach -c "read8 0x2000593c 2200"
      ```
      ⚠ Read-only commands only. Do **not** `halt` while the SoftDevice runs.
      Confirmed safe this run: repeated `read8`/`read32` in `attach` mode left
      the heartbeat monotonic across many invocations.
- [x] ANT network key provisioned; real Garmin previously paired to footpod
      #45694 (device type 124, Stride SDM).
- [x] LF clock on the 32.768 kHz crystal (`LFCLKSTAT = 0x00010001`, SRC:Xtal).

### Reading device state directly out of RAM

`app_state_t` (`s_state`, 52 bytes) was the primary instrument, and is more
direct evidence than a log line. Note `link_state_t` **packs to one byte** —
arm-none-eabi-gcc defaults to `-fshort-enums`:

| Offset | Addr | Field |
|---|---|---|
| 0  | `0x20004614` | `treadmill.speed_mps` (float) |
| 4  | `0x20004618` | `treadmill.distance_m` (float) |
| 8  | `0x2000461c` | `treadmill.incline_pct` (float) |
| 12 | `0x20004620` | `treadmill.elapsed_s` (u32) |
| 16 | `0x20004624` | `central_link` (**1 byte**: 0=DOWN 1=SCAN 2=CONN 3=UP) |
| 17 | `0x20004625` | `treadmill_name[24]` |
| 41 | `0x2000463d` | `watch_connected` (bool) |
| 42 | `0x2000463e` | `ant_broadcasting` (bool) |
| 44 | `0x20004640` | `resolved_target_mps` (float) |
| 48 | `0x20004644` | `last_fault_code` (u32) |

## Part A -- all three radios confirmed on-device

### Check A1: BLE peripheral (ctrl link) is up

- [x] Watch mock connects and subscribes.
  - Result: **PASS**
  - Evidence: `ctrl_svc: watch connected (handle 1) int=24 lat=0 sup=72`,
    then `ctrl_svc: notifications on`. `watch_connected = 0x01` in RAM.

### Check A2: BLE central scan + connect

- [x] Scan discovers the treadmill and reports it.
  - Result: **PASS** (real iFit, not the mock)
  - Response: `ctrl_svc: rx "SCAN"` → `central: scanning for treadmills...` →
    `ctrl: {"cmd":"scan","ok":true}` → `central: found "" rssi -60 (iFit)`
  - ⚠ Note: the `found` line reports an **empty name**; the name `I_TL` only
    appears at connect time. See "Defects found" #3.

- [x] Connect establishes a usable link.
  - Result: **PASS**, but **only on the 5th attempt** — see "Defects found" #1.
  - Response: `central: connected "I_TL" (iFit, handle 0)` →
    `service handles 9-14` → `notify char @11` → `control char @14` →
    `CCCD @12` → `central: subscribed — notifications active` →
    `last_device: saving "I_TL"` → `central: iFit init+keepalive started`
  - `central_link = 0x03` (LINK_UP), `treadmill_name = "I_TL"` in RAM.

### Check A3: Belt-speed control write lands on the treadmill

- [x] Target set from the watch mock propagates to the real belt.
  - Result: **PASS**
  - Speed requested: **8.0 km/h** (`TARGET 8.0` from `mock_watch.py`)
  - Speed observed: **7.99 km/h** on the real belt
  - Evidence, read from RAM (the `A6ED0004` path logs nothing — see
    "Defects found" #2, so RAM was the only instrument):
    `resolved_target_mps` went `0.0` → `0x400E3540` = 2.2217 m/s = **8.00 km/h**;
    `treadmill.speed_mps` = `0x400E0B61` = 2.2193 m/s = **7.99 km/h**.
    `distance_m` integrated continuously (291.6 m → 384.8 m over the sample).
  - Chain proven end to end: mock watch → 15-byte frame to `A6ED0004` →
    `workout_ctrl` → resolved target → iFit control write → real belt speed.

### Check A4: ANT master broadcasting (footpod)

- [x] ANT channel opens for the SDM (device type 124).
  - Result: **PASS**
  - Log excerpt: `ant_sdm init, dev_num=45694` / `ant_sdm broadcasting` /
    `ant_sdm: initialized and broadcasting`. `ant_broadcasting = 0x01` in RAM.

- [x] Broadcast is live alongside both BLE links.
  - Result: **PASS**
  - Broadcast interval: **not independently measured this run.** Channel period
    8192 implies 32768/8192 = 4 Hz by construction. The ANT leg was proven
    end-to-end earlier by a real Garmin pairing to footpod #45694; this run
    confirms only that the channel is open and the flag is set while both BLE
    links are active.

### Check A5: All three radios up simultaneously

- [x] All three live, no faults.
  - Result: **PASS**
  - Duration: **~366 s continuous** (subscribed at `alive ~55`, still healthy at
    `alive 421`), against a 60 s requirement. 43 s of that was sampled at ~3 s
    intervals with every sample reading `link=UP`, `belt=7.99`, `target=8.00`,
    `fault=0`.
  - Notes: no hard-fault, no assert, no disconnect on either BLE link, no ANT
    drop. `last_fault_code = 0` throughout. USB-CDC stall is not applicable —
    USB enumeration is broken independently of this gate.

## Part B -- real treadmill + real Garmin

### Check B1: BLE peripheral (real watch link) is up

- [ ] Result: **NOT RUN** — `mock_watch.py` was used as the peripheral peer.

### Check B2: BLE central (real treadmill link) is up

- [x] Result: **PASS**
  - Treadmill make/model: real iFit machine, advertises as `I_TL`
  - Protocol negotiated: **iFit** (`0x1533`)
  - GATT: service handles 9-14, notify @11, control @14, CCCD @12.
    Telemetry flowing; `distance_m` integrated from speed as designed
    (iFit frames carry no distance).

### Check B3: Belt-speed control (real treadmill)

- [x] Result: **PASS** — same evidence as A3; the treadmill in A3 *was* the
  real iFit machine.
  - Target speed: 8.0 km/h → actual belt 7.99 km/h (within 0.01 km/h)

### Check B4: ANT+ footpod pace readback on the watch

- [ ] Result: **NOT RUN concurrently.** The real Garmin paired to footpod
  #45694 in an earlier session, but pace-tracks-belt was not verified while
  the treadmill link was up. Needs the real watch.

### Check B5: End-to-end concurrency (60-second continuous run)

- [ ] Result: **NOT RUN** — needs the real Garmin watch (blocked with B1/B4).
  Note the equivalent run with a mock watch (A5) passed for ~366 s.

## Summary

| Check | Description | Result |
|-------|-------------|--------|
| A1    | BLE peripheral (mock watch connected) | **PASS** |
| A2    | BLE central scan + connect (**real iFit**) | **PASS** (5th attempt) |
| A3    | Belt-speed control lands on treadmill | **PASS** (8.00 → 7.99 km/h) |
| A4    | ANT master broadcasting (footpod) | **PASS** |
| A5    | All three radios up simultaneously | **PASS** (~366 s) |
| B1    | BLE peripheral (real watch connected) | NOT RUN |
| B2    | BLE central (real treadmill connected) | **PASS** (iFit `I_TL`) |
| B3    | Belt-speed control (real treadmill) | **PASS** |
| B4    | ANT+ footpod pace readback on watch | NOT RUN |
| B5    | End-to-end concurrency (60 s, real HW) | NOT RUN |

**Overall gate result: PASS Part A (with the treadmill leg on real hardware).
Part B outstanding: B1, B4, B5 — all blocked only on the real Garmin watch.**

The core architectural risk this gate exists to test — S340 BLE peripheral +
BLE central + ANT master coexisting — is **cleared**. What remains is peer
compatibility with the real watch, not concurrency.

Operator: alex
Date: 2026-07-29
Firmware commit: `835f588` + 4 uncommitted files (listed at the top)

---

## Fixes verified by this run

All four uncommitted files were made after the previous session's handoff and
had never been tested on hardware. Three are now confirmed; one is untested.

1. **`ble_ctrl_svc.c` — PPCP + supervision-timeout renegotiation: VERIFIED, and
   it was the real bug.** macOS offered `sup=72` (720 ms), exactly as the code
   comment predicted; the device asked for 400 and the central accepted:
   ```
   ctrl_svc: watch connected (handle 1) int=24 lat=0 sup=72
   ctrl_svc: sup=72 too short — requested 400
   ctrl_svc: conn params now int=48 lat=0 sup=400
   ```
   The watch link then survived ~670 s on the previous boot and ~366 s on this
   one, through **five** central connect/disconnect cycles, with zero
   `BLE_HCI_CONNECTION_TIMEOUT (0x08)`. That starvation drop was the failure
   this whole change set targeted, and it is gone.

2. **`ble_central.c` — save last-device only after `subscribed()`: VERIFIED.**
   The connects that died with `0x3E` produced **no** `last_device: saving`
   line; only the one that reached `subscribed` did. A link that proved nothing
   no longer becomes "the saved device".

3. **`app_config.h` — scan duty cycle 50%→20%, central conn interval
   7.5→30/60 ms: VERIFIED indirectly.** No direct measurement, but the watch
   link surviving the connect loop in #1 is the intended effect.

4. **`ble_central.c` — 30 s discovery-failure cooldown: NOT EXERCISED.** GATT
   discovery never failed, so `suppressing it for 30 s` never fired. This path
   remains untested, and per "Defects found" #1 it does not cover the failure
   mode actually observed.

## Defects found during this run

1. **Unbounded tight reconnect loop on `0x3E`
   (`BLE_HCI_CONN_FAILED_TO_BE_ESTABLISHED`).** Observed four consecutive
   failures before the fifth attempt succeeded:
   ```
   connecting to "I_TL" (iFit) → connected "I_TL" (iFit, handle 0)
     → disconnected (reason 0x3E) → scanning for treadmills... → (repeat)
   ```
   `ble_central.c:828` (`BLE_GAP_EVT_DISCONNECTED`) calls
   `ble_central_scan_start()` on **every** disconnect regardless of reason, with
   no backoff; `policy_evaluate()` then re-picks the saved device on the very
   next advert. A machine that never establishes would loop forever. The new
   30 s cooldown does **not** help — it arms only in `gattc_fail()` (a GATT
   *discovery* failure), and `0x3E` is an *establishment* failure on a different
   path.
   Suggested fix: treat **any** disconnect that occurred before `s_stage`
   reached `DISC_DONE` as a failed attempt and apply an escalating per-address
   backoff (1/2/4/8 s, capped at 30 s), unifying the `0x3E` and `gattc_fail`
   paths. A link that *did* reach `DISC_DONE` should still reconnect
   immediately, preserving fast recovery on a genuine treadmill drop
   (hardware-test-plan Phase 8.1).
   Root cause of the `0x3E` itself is **not** established — candidates include
   radio contention from three concurrent radios, a stale link held by the
   treadmill after the board was reset out from under it, and marginal RSSI
   (-60 to -70 observed).

2. **`A6ED0004` workout-frame writes are completely unlogged.**
   `ble_ctrl_svc.c:210` calls `workout_ctrl_on_frame()` with no `NRF_LOG`,
   while every sibling input logs (`rx "SCAN"`, `notifications on`). The single
   most important product path — watch sets target → belt follows — leaves no
   trace, which is why A3 had to be verified by reading RAM. Add a log line.

3. **Advert names come through empty.** `central: found "" rssi -60 (iFit)` —
   every `found` line this run had an empty name, yet `I_TL` is known by connect
   time. `LIST` sent to the watch would therefore show blank names, making the
   device picker unusable on the real watch. Likely the name is in the scan
   response rather than the primary advert payload. **This should be fixed
   before B1/B5**, since the watch UI depends on it.

4. **A ctrl reply was truncated in the log:** `ctrl: {"cmd":"sto`, for the
   `STOP` reply, despite `ble_ctrl_svc.c:194` correctly using `nrf_log_push`.
   Sibling replies (`scan`, `connect`) logged in full. Cosmetic so far, but
   unexplained.

5. **An unidentified FTMS advertiser with an empty name** appeared on the first
   boot (`found "" rssi -67 (FTMS)`); an auto-connect to it timed out
   (`connect timed out — rescanning`) before the operator's `CONNECT 0`
   redirected to the iFit machine. Possibly a leftover `mock_treadmill.py`, or a
   neighbouring device. Worth identifying — it caused a wasted 5 s connect
   attempt during a live session.

## Notes for the next run

- `RESETREAS` (`0x40000400`) is the fastest way to tell a crash from a button
  press: `0x1` = RESETPIN (operator), `0x2` = watchdog, `0x4` = soft reset
  (`NVIC_SystemReset`, i.e. the testboard long-press or a fatal error), `0x8` =
  lockup. A power-on reset reads `0x0`. It accumulates and is never cleared in
  software, so read it as "what has happened since power-on".
- The RTT ring is 8 KB and `RdOff` stays 0 with nothing draining it, so a whole
  boot fits and stays readable. Track `WrOff` (`0x2000802c`) between reads to
  find just the new output.
- Run the host mocks from a **real terminal**: macOS denies Bluetooth to a
  process with no controlling TTY, and the failure is silent.
