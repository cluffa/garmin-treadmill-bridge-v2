# nRF52840 Treadmill Bridge v2 — Claude Code Instructions

## Build & flash

```sh
# Host tests (core/ only, no firmware)
make host-test

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
make flash-app                         # fast app-only reflash + settings page (SWD)
make flash-sd                          # SoftDevice only, chip-erases (⚠ see below)
make dfu                               # signed USB-DFU package (consumes build)
make dfu-enter                         # kick running app into DFU (SWD, GPREGRET)
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

- BLE service: `A6ED0001-2E7A-4E1D-9E3B-000000000000`
- Char `A6ED0002` write: uppercase ctrl grammar (`SPEED`, `SCAN`, `CONNECT`, `STOP`, `LIST`, `STATUS`).
- Char `A6ED0003` notify: compact `D`/`E`/`S` frames (CIQ MTU is 23; notify payload <= 20 bytes).
- Char `A6ED0004` write: raw 15-byte little-endian workout frame -> `workout_ctrl`.

## One-connection-at-a-time invariant

The BLE central holds at most one treadmill link; connecting tears down the other protocol first
and suppresses auto-reconnect while the other is connecting/connected. Never reintroduce
simultaneous FTMS+iFit connections.

## ANT

- Device type 124 (Stride SDM).
- Network key in git-ignored `ant_network_key.h` (placeholder builds; real key from thisisant.com).
- ANT eval license key passed by Makefile.

## Secrets (never committed)

- `ant_network_key.h` — see `ant_network_key.h.example`.
- `ant_license.mk` — see `ant_license.mk.example`.
- `dfu/dfu_private_key.pem` — see `dfu/dfu_private_key.pem.example`.

## Architecture

`core/` is platform-agnostic protocol logic (parsers, FSM, belt-control policy, ANT SDM encoding).
`firmware/` is the nRF5-SDK + S340 radio glue: BLE peripheral (watch-facing ctrl-svc), BLE central (treadmill-facing FTMS/iFit), ANT master (footpod broadcast), USB-CDC console.
The bridge between them is `core/machine.h` — a unified facade that auto-detects FTMS (0x1826) and iFit (0x1533) into one device list and routes connect/speed/incline/stop to the right adapter.
`firmware/app_state.h` is the shared struct all three radios and the testboard render from.

## `firmware/` Makefile TESTBOARD stamp

The Makefile auto-detects `TESTBOARD` changes between builds and force-cleans objects.
Don't be surprised by a clean rebuild when you toggle between `TESTBOARD=0` and `TESTBOARD=1`.

## Source of truth for vendored files

The old repo at `/Users/alex/workspace/nrf52/garmin-treadmill-bridge/` (referred to as `$OLD`).

## Finishing plan

All code milestones (M0-M4) are complete as of `fc5ac15`. The only remaining step is the
M4.2 three-radio concurrency gate on physical hardware. See `docs/finishing-plan.md`.
