# Concurrency Gate -- 2026-07-21

**IMPORTANT: This test MUST be run on hardware by the operator.**
All fields marked `___` are to be filled in during the test session.

## Purpose

Prove that all three radios (BLE peripheral, BLE central, ANT master) operate
concurrently under the S340 SoftDevice without deadlocks, timeouts, or
hard-faults. This is the make-or-break gate for the v2 "device-is-the-brain"
architecture.

## Prerequisites

Status as of 2026-07-27:

- [x] XIAO nRF52840 flashed with the latest `firmware/_build/nrf52840_xxaa.hex`
      — `make flash-full TESTBOARD=1`, boots clean, heartbeat steady at 1 Hz.
- [x] S340 SoftDevice pre-flashed — v7.0.1; both stacks report enabled
      (`BLE stack enabled`, `ANT stack enabled`).
      **Do NOT use `make flash-sd`**: it chip-erases the bootloader and UICR
      and is what broke this board earlier. `make flash-full` covers it.
- [ ] ~~USB-CDC console~~ **UNAVAILABLE — USB enumeration is broken**
      (see `docs/HANDOFF.md`). The device serves descriptors but exposes zero
      interfaces, so there is no `/dev/cu.usbmodem*` for it and `screen` is not
      an option. **Use RTT over the SWD probe instead** — it is strictly better
      for this test anyway, and is how every diagnosis this session was made:
      ```sh
      nm -S firmware/_build/nrf52840_xxaa.out | grep _acUpBuffer   # e.g. 2000592c
      pyocd commander -t nrf52840 -O connect_mode=attach -c "read8 0x2000592c 2048"
      ```
      ⚠ Use read-only commands. Do **not** `halt` the core while the SoftDevice
      is running — it trips an SD assert and the board dies a few seconds later,
      which will read as a concurrency failure that isn't one.
- [x] ANT network key provisioned — real key in place (git-ignored), and a real
      Garmin watch pairs and connects to footpod #45694 (device type 124,
      Stride SDM). This means Check A4 is already proven independently.
- [x] LF clock on the 32.768 kHz crystal (`LFCLKSTAT = 0x00010001`, SRC:Xtal),
      20 ppm rather than the RC's 500 ppm. Relevant here: LF accuracy sets the
      BLE connection-event and ANT channel timing margin this gate is testing.

## Part A -- Mocks (all three radios confirmed on-device)

This stage uses `mock_watch.py` and `mock_treadmill.py` as the remote endpoints
so we can validate all three radios are up before adding real treadmill/watch
variables.

### Setup

1. Start `mock_treadmill.py` on the host:
   ```sh
   cd test/mock && uv run mock_treadmill.py
   ```
   The mock advertises FTMS (0x1826) and prints `tx speed=...` every 500 ms.

2. Start `mock_watch.py` on the host (a different terminal):
   ```sh
   cd test/mock && uv run mock_watch.py --target 8.0
   ```
   The mock connects to the bridge's A6ED control service and subscribes to
   notifications (0x0003).

3. Confirm both mocks are running and the watch mock is connected.

### Check A1: BLE peripheral (ctrl link) is up

- [ ] Watch mock prints `connected: True` and `subscribed`.
  - Result: `___` (PASS / FAIL)
  - Time: `___`

### Check A2: BLE central scan + connect

- [ ] In the watch mock terminal, type `LIST`. Confirm a `D` frame returns with
  the mock treadmill's name and RSSI.
  - Result: `___` (PASS / FAIL)
  - Response: `___`

- [ ] In the watch mock terminal, type `SCAN` then `CONNECT 0`. Confirm an `S`
  frame arrives showing `connected` with the mock treadmill.
  - Result: `___` (PASS / FAIL)
  - Response: `___`

### Check A3: Belt-speed control write lands on mock treadmill

- [ ] In the watch mock terminal, type `TARGET 10.5` (or just `T` for the
  default 8.0 km/h). Confirm:
  1. The watch mock log shows `=> wkt <hex>` (15-byte frame sent to 0x0004).
  2. The mock treadmill terminal prints `CP -> set speed 10.50 km/h` (or the
     appropriate target speed, confirming the nRF52840 decoded the frame and
     issued an FTMS control-point write).
  - Result: `___` (PASS / FAIL)
  - Speed requested: `___` km/h
  - Speed observed on treadmill mock: `___` km/h

### Check A4: ANT master broadcasting (footpod)

- [ ] Verify the USB-CDC console log shows an ANT channel-open event for the SDM
  (device type 124). Look for a log line containing `ant_sdm` or `SDM` or the
  channel-open success return code.
  - Result: `___` (PASS / FAIL)
  - Console log excerpt: `___`

- [ ] With the ANT channel open, verify that the broadcast interval is being hit
  (the firmware should log or the `mock_watch.py` subscribe handler should show
  no errors from the central side).
  - Result: `___` (PASS / FAIL)
  - Broadcast interval observed: `___` Hz / ms

### Check A5: All three radios up simultaneously

- [ ] While the ANT channel is broadcasting AND the BLE central is connected to
  the mock treadmill AND the BLE peripheral link to the watch mock is active,
  run for at least 60 seconds without:
  - A hard-fault or assert
  - A SoftDevice timeslot conflict error
  - A BLE disconnection (any link)
  - A USB-CDC stall
  - Result: `___` (PASS / FAIL)
  - Duration: `___` s
  - Notes: `___`

## Part B -- Real treadmill + real Garmin

**Only proceed after Part A passes completely.**

### Setup

1. Power on the treadmill and verify it advertises FTMS (or iFit, if that is
   the target protocol).
2. Put on the Garmin watch with the Connect IQ data field installed.
3. Verify the nRF52840 is within 2 meters of both the treadmill and the watch.

### Check B1: BLE peripheral (watch link) is up

- [ ] The Garmin watch's data field connects to the bridge. Confirm the USB-CDC
  console shows a BLE connection event with the watch's address.
  - Result: `___` (PASS / FAIL)
  - Watch model / firmware: `___`

### Check B2: BLE central (treadmill link) is up

- [ ] From the watch, trigger a `SCAN` then `CONNECT` (or accept the saved
  device). Confirm:
  1. The USB-CDC console shows the treadmill listed in the scan results.
  2. After connect, the console shows the FTMS/iFit link established.
  - Result: `___` (PASS / FAIL)
  - Treadmill make/model: `___`
  - Protocol negotiated: `___` (FTMS / iFit)

### Check B3: Belt-speed control

- [ ] From the watch, initiate a workout that sets a target speed (e.g., 8
  km/h). Confirm:
  1. The USB-CDC console shows the decoded telemetry frame (speed target).
  2. The treadmill's actual belt speed ramps to the target.
  3. The treadmill's display matches the target speed within 0.5 km/h.
  - Result: `___` (PASS / FAIL)
  - Target speed: `___` km/h
  - Actual belt speed: `___` km/h

### Check B4: ANT+ footpod pace readback on the watch

- [ ] With the ANT channel open and the nRF52840 broadcasting footpod data:
  1. The Garmin watch displays a pace/speed field that tracks the treadmill's
     belt speed (within reasonable accuracy).
  2. The watch does NOT show "No Foot Pod" or a sensor-disconnected status.
  - Result: `___` (PASS / FAIL)
  - Pace shown on watch: `___` min/km
  - Treadmill belt speed: `___` km/h
  - Error (if any): `___`

### Check B5: End-to-end concurrency (60-second continuous run)

- [ ] Run for at least 60 seconds with:
  - Watch connected (BLE peripheral, A6ED)
  - Treadmill connected (BLE central, FTMS or iFit)
  - ANT footpod broadcasting (ANT master)
  - At least one speed change during the run
  - [ ] No hard-fault / assert
  - [ ] No BLE disconnection (watch or treadmill)
  - [ ] No ANT channel drop
  - [ ] No USB-CDC stall
  - [ ] Belt speed tracked the requested speed through the change
  - [ ] Watch pace updated within 2 seconds of the belt-speed change
  - Result: `___` (PASS / FAIL)
  - Duration: `___` s
  - Speed change at t=`___` s from `___` to `___` km/h, belt followed at t=`___` s
  - Notes: `___`

## Summary

| Check | Description | Result |
|-------|-------------|--------|
| A1    | BLE peripheral (mock watch connected) | `___` |
| A2    | BLE central scan + connect (mock treadmill) | `___` |
| A3    | Belt-speed control write lands on mock treadmill | `___` |
| A4    | ANT master broadcasting (footpod) | `___` |
| A5    | All three radios up simultaneously (mocks) | `___` |
| B1    | BLE peripheral (real watch connected) | `___` |
| B2    | BLE central (real treadmill connected) | `___` |
| B3    | Belt-speed control (real treadmill) | `___` |
| B4    | ANT+ footpod pace readback on watch | `___` |
| B5    | End-to-end concurrency (60 s, real HW) | `___` |

**Overall gate result: `___` (PASS all A+B / PASS A only / FAIL)**

Operator: `___`
Date: `___`
Firmware commit: `___`
