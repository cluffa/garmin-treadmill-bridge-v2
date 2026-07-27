# v2 Finishing Plan — July 2026

## Status: Implementation Complete

All milestones M0-M4 are code-complete, build-clean, and host-tested. Commit `fc5ac15`.

- **make host-test**: 9/9 OK
- **make -C firmware TESTBOARD=0**: DONE (~82 KB text, indicative — from commit `fc5ac15`)
- **make -C firmware TESTBOARD=1**: DONE (~92 KB text, indicative — from commit `fc5ac15`)
- **make dfu**: signed package produced (`--sd-req 0xCE`)
- **Secrets**: clean; no real keys in tracked files

## Single Remaining Task: M4.2 Three-Radio Concurrency Gate

This is inherently **hardware-only** (S340 radio cannot run under Renode). No code changes expected.

> **First-silicon bring-up is DONE (2026-07-22).** The S340+app boots on real
> XIAO hardware (BLE advertising, ANT broadcasting, OLED, USB-CDC console), and
> the full SWD + USB-DFU flashing path is verified. See `docs/flashing.md` for
> the complete, hardware-tested procedure (this section is a summary).

### Step 1 — Hardware Pre-requisites

- [ ] Physical XIAO nRF52840 board + Pico CMSIS-DAP SWD probe (wiring: `docs/flashing.md` §2)
- [ ] `firmware/ant_license.mk` exists with real `ANT_LICENSE_KEY` (copy from `.example`)
- [ ] `firmware/ant_network_key.h` exists with real ANT+ network key (copy from `.example`)
- [ ] `dfu/dfu_private_key.pem` exists for DFU signing (generate: `nrfutil keys generate`)

### Step 2 — Flash (see `docs/flashing.md` for full detail)

```sh
make -C firmware -j8 \
  GNU_INSTALL_ROOT=/path/to/arm-none-eabi-gcc/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/path/to/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/path/to/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include

# First-time provisioning (SoftDevice + app + bootloader + settings, over SWD):
make flash-full

# Subsequent updates over USB (no SWD): build pkg, enter DFU, push it:
make dfu
make dfu-enter                          # or let an invalid app auto-enter DFU
make flash-dfu SERIAL=/dev/cu.usbmodemXXXX
# Fast SWD app-only reflash (+ settings): make flash-app
```

### Step 3 — Run the Concurrency Gate

The full checklist is in `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`.

**Part A (mocks, no treadmill/watch needed):**
1. Start `mock_treadmill.py` (fake treadmill, FTMS peripheral)
2. Start `mock_watch.py --target 8.0` (fake watch)
3. Verify A1: BLE peripheral link up (watch mock connected)
4. Verify A2: BLE central scan + connect (LIST, SCAN, CONNECT 0)
5. Verify A3: Belt-speed control write lands on mock (`TARGET 10.5`)
6. Verify A4: ANT master broadcasting (footpod, device type 124)
7. Verify A5: All three radios up simultaneously for 60+ seconds, no faults

**Part B (real treadmill + real Garmin watch):**
1. Verify B1: Real watch connects over BLE peripheral
2. Verify B2: Real treadmill connects over BLE central (FTMS or iFit)
3. Verify B3: Belt-speed control works end-to-end
4. Verify B4: ANT+ footpod pace shows on watch
5. Verify B5: 60-second continuous run, all three radios, with speed change

### Step 4 — If Something Fails

- Check USB-CDC console (`screen /dev/cu.usbmodemXXXX 115200`) for log output
- ANT channel open failure: verify `ANT_LICENSE_KEY` and `ant_network_key.h` are correct
- BLE central won't connect: verify treadmill is advertising FTMS (0x1826) or iFit (0x1533)
- Hard fault: S340 timeslot conflicts are the most likely suspect; check SDK config for concurrent BLE+ANT resource allocation
- If issues arise, the code is in `firmware/ant_sdm.c` (ANT master), `firmware/ble_central.c` (BLE central), `firmware/ble_ctrl_svc.c` (BLE peripheral)

### Step 5 — Completion

- Fill in the gate checklist results in `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`
- Overall gate result: PASS all A+B, or PASS A only, or FAIL
- If PASS: commit the filled checklist, tag a release, project is done
- If FAIL: document the failure mode and triage
