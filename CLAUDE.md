# nRF52840 Treadmill Bridge v2 — Claude Code Instructions

## Build & flash

```sh
# Host tests (core/ only, no firmware)
make host-test

# Firmware (nRF52840 + S340)
make -C firmware -j8 \
  GNU_INSTALL_ROOT=/Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/Users/alex/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include

# DFU packaging
make -C firmware dfu

# Flash (requires hardware)
make -C firmware flash-dfu SERIAL=/dev/cu.usbmodemXXXX
make -C firmware flash-sd          # one-time SoftDevice flash
make -C firmware flash-app         # SWD fallback
```

## Toolchain

- **Host C compiler:** cc/gcc/clang (for `core/` + `test/host/`).
- **Firmware ARM toolchain:** PlatformIO's GCC 7.2.1 (SDK-compatible), NOT Homebrew's 16.1.0:
  `GNU_INSTALL_ROOT=/Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/  GNU_VERSION=7.2.1`
- **nRF5 SDK 17.1.0:** `SDK_ROOT=/Users/alex/nRF5_SDK_17.1.0_ddde560`
- **S340 v7.0.1 API headers:** `S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include`
- **S340 hex:** `/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex` (S340 is NOT in the SDK's softdevice dir)

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
- `dfu/dfu_private_key.pem` — see `dfu/dfu_private_key.pem.example`.

## Source of truth for vendored files

The old repo at `/Users/alex/workspace/nrf52/garmin-treadmill-bridge/` (referred to as `$OLD`).
