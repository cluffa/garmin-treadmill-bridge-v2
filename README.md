# garmin-treadmill-bridge-v2

An nRF52840 (Seeed Studio XIAO nRF52840) bridge that lets a Garmin watch drive a
treadmill's belt and read pace -- with the **device as the brain**.

The nRF52840 owns all belt-control policy (speed ramp, hold, changes), so the
watch ships a compact 15-byte telemetry frame over BLE and reads pace natively
over ANT+. Three radios run **concurrently** under the S340 SoftDevice.

```
                BLE peripheral                        BLE central
  Watch         (A6ED GATT server,                    (FTMS / iFit client)
  (Garmin)      15-byte ctrl frame)   nRF52840        Treadmill
 ┌──────┐      ┌──────────────────────┐  ┌─────────┐  ┌──────────┐
 │      │ BLE  │ A6ED0001 svc          │  │         │  │ FTMS 0x  │
 │ CIQ  │◄────►│  A6ED0002 write ctrl  │  │workout  │  │ 1826,    │
 │ data │      │  A6ED0003 notify resp │  │_ctrl    │──│ iFit     │
 │ field│      │  A6ED0004 write wkt   │  │         │  │ control  │
 │      │      ├───────────────────────┤  │         │  └──────────┘
 │      │ ANT+ │ ANT master (SDM)      │  │         │
 │      │◄─────│  device type 124      │  │         │
 │      │ foot │  broadcasts pace every│  │         │
 │      │ pod  │  stride              │  │         │
 └──────┘      └──────────────────────┘  └─────────┘
```

All three radios run concurrently via the **S340 SoftDevice** (BLE peripheral,
BLE central, ANT master).

## Hardware

- **Seeed Studio XIAO nRF52840** (nRF52840, 1 MB flash, 256 KB RAM)
- **Optional -- expansion board** (compile-time `TESTBOARD=1`): SSD1306 128x64
  OLED, button, passive buzzer, onboard RGB LED. This is a bring-up aid -- the
  production build compiles with `TESTBOARD=0`.

## Repo layout

| Directory      | Purpose |
|----------------|---------|
| `core/`        | Proven, host-tested protocol code (belt control policy, frame parsing, FSM). No nRF/SDK/BLE dependencies -- compiles for the host. |
| `firmware/`    | nRF52840 / S340 radio glue: BLE GATT server (peripheral), BLE central (FTMS/iFit), ANT master (SDM footpod), USB-CDC console. |
| `test/host/`   | 9 C unit-test suites for `core/` (the green gate -- must pass before every commit). |
| `test/mock/`   | Python BLE mocks that let you test the device against a fake watch (`mock_watch.py`) and a fake treadmill (`mock_treadmill.py`) without hardware. |
| `test/renode/` | Bounded Renode non-radio smoke test (ELF loads, CPU starts). The S340 radio cannot run under Renode -- this is a smoke test, not a gate. |
| `dfu/`         | DFU key management and documentation (see `dfu/README.md`). |
| `docs/`        | Superpowers plans, specs, and test-log templates. |

## Build and flash

See `CLAUDE.md` for the canonical environment variables and toolchain setup.

### Host unit tests (no hardware)

```sh
make host-test
```

Expect 9 suites, all OK.

### First build on a new machine

A fresh clone **cannot build** until two git-ignored files are created from
their `.example` templates:

```sh
cp firmware/ant_network_key.h.example firmware/ant_network_key.h
cp firmware/ant_license.mk.example   firmware/ant_license.mk
```

- `ant_network_key.h` — ANT+ network key (obtain from
  [thisisant.com](https://www.thisisant.com)). Without it the build fails with
  `fatal error: ant_network_key.h: No such file or directory`.
  **The template is eight zero bytes, which builds but does not interoperate.**
  A zero network key is not the ANT+ network, so the footpod broadcast will not
  be recognised by a watch or any ANT+ receiver — the radio transmits and
  nothing pairs. Paste the real key before you expect ANT+ to work; otherwise
  the ANT leg of the concurrency gate fails in a way that looks like radio
  contention.
- `ant_license.mk` — ANT evaluation license key (16-byte hex, also from
  thisisant.com) and the matching `CFLAGS` assignment. Without it
  `sd_ant_enable()` fails at runtime.

Signing DFU packages additionally requires `dfu/dfu_private_key.pem`
(see `dfu/README.md`).

### Firmware build

```sh
make -C firmware -j8 \
  GNU_INSTALL_ROOT=/path/to/arm-none-eabi-gcc/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/path/to/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/path/to/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include
```

**Toolchain note:** You MUST use PlatformIO's GCC 7.2.1 (the SDK is not
compatible with Homebrew's `arm-none-eabi-gcc` 16). See `CLAUDE.md` for the
canonical variable values and this machine's concrete paths.

Or use the top-level convenience target (forwards `?=` defaults for all four
variables — override with e.g. `make SDK_ROOT=/elsewhere firmware`):

```sh
make firmware
```

### Flashing

The XIAO nRF52840 has **no onboard debugger**. All flashing goes through
either:

- **SWD:** a Raspberry Pi Pico running CMSIS-DAP firmware, driven by
  **pyOCD** (NOT `nrfjprog` / J-Link).
- **USB:** the Nordic Secure USB-DFU bootloader, driven by **nrfutil**.

The full authoritative guide (wiring, memory map, provisioning, everyday
loops, troubleshooting) is **`docs/flashing.md`**. Quick reference:

```sh
# Build first, then flash:
make firmware

# First-time provisioning (SD+app+bootloader+settings, chip erase):
make flash-full
# ⚠ make flash-sd chip-erases — it wipes the bootloader, settings, and UICR.
#   Prefer flash-full for first-time setup, flash-app for updates.

# Fast app reflash over SWD (+ refreshed settings page):
make flash-app

# USB-DFU loop (build first, then):
make dfu               # signed package → firmware/_build/app_dfu.zip
make dfu-enter          # kick running app into DFU (needs SWD probe)
make flash-dfu SERIAL=/dev/cu.usbmodemXXXX
```

All flash/DFU targets **consume an existing build** — they do not rebuild.
Build first, then flash.

## Testing layers

1. **Host unit tests** (green gate). `make host-test` -- 9 suites, must pass
   before every commit.
2. **Host BLE mocks** (functional integration). Run `mock_treadmill.py` (fake
   treadmill, FTMS peripheral) and `mock_watch.py` (fake Garmin watch/central)
   via `uv run`, then exercise the bridge against them. No hardware needed.
3. **Renode non-radio smoke** (bounded). Proves the ELF loads and the CPU
   starts. The S340 SoftDevice radio is NOT emulable in Renode, so this is a
   smoke test, not a gate.
4. **Real-hardware 3-radio concurrency gate** (make-or-break). Run on the
   physical XIAO nRF52840 with a real treadmill and real Garmin watch, OR with
   the mocks and a real device for staged bring-up. See
   `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`.

## Control contract

The nRF52840 exposes a GATT server that the watch's Connect IQ data field or
picker app connects to:

- **Service:** `A6ED0001-D344-460A-8075-B9E8EC90D71B`
- **Characteristic `A6ED0002`** -- write. Uppercase control grammar:
  `STATUS`, `LIST`, `SCAN`, `CONNECT <n>`, `SPEED <km/h>`, `STOP`.
- **Characteristic `A6ED0003`** -- notify. Compact `D`/`E`/`S` response frames
  (CIQ MTU is 23 bytes; notify payload <= 20 bytes).
- **Characteristic `A6ED0004`** -- write. Raw 15-byte little-endian workout
  telemetry frame consumed by `core/workout_ctrl.c`. The device then translates
  this into FTMS/iFit belt-control commands.

Only one BLE-central connection at a time: connecting tears down the other
protocol first and suppresses auto-reconnect while the other is
connecting/connected.

## Secrets and licensing

| Secret | Placeholder | Notes |
|--------|-------------|-------|
| ANT+ network key | `firmware/ant_network_key.h.example` | Real key must be obtained from [thisisant.com](https://www.thisisant.com) (free for personal use). The git-ignored `ant_network_key.h` is required to open the ANT SDM channel. |
| DFU private key | `dfu/dfu_private_key.pem.example` | Generate with `nrfutil keys generate`. The matching public key (`dfu/dfu_public_key.c`) IS committed -- it is compiled into the bootloader. The private key is git-ignored. |
| ANT eval license key | `firmware/ant_license.mk.example` | A 16-byte hex string + `CFLAGS` assignment, passed as `-DANT_LICENSE_KEY`. The key is **not** checked in — it lives in the git-ignored `firmware/ant_license.mk`. Obtain from [thisisant.com](https://www.thisisant.com). A commercial release requires a licensed key. |

No real keys, tokens, or UUIDs-as-secrets are committed.

## Built with AI assistance

This project was built with AI assistance (Claude Code / Anthropic Claude).

## License

See [LICENSE](./LICENSE).
