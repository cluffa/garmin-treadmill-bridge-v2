# Flashing & USB-DFU Guide — XIAO nRF52840 + S340

This is the authoritative, hardware-verified procedure for programming the board.
The Seeed XIAO nRF52840 has **no onboard debugger**, so J-Link / `nrfjprog` are
**not** usable. We flash over SWD with a **Raspberry Pi Pico running the CMSIS-DAP
"debugprobe" firmware**, driven by **pyOCD**. Over-the-air-style updates go through
the **Nordic Secure USB-DFU bootloader** with `nrfutil`.

- Build command with all required toolchain flags: see §4a below.
- Tools used here: `pyocd`, `nrfutil` (Rust plugin build, `nrf5sdk-tools`), installed at `~/.local/bin`.

---

## 1. Flash memory map (nRF52840, 1 MB)

| Region              | Address range        | Contents                              |
|---------------------|----------------------|---------------------------------------|
| MBR                 | `0x00000`–`0x00B00`  | Master Boot Record (part of S340 hex) |
| SoftDevice (S340)   | `0x01000`–`0x305D0`  | ANT + BLE combined stack, v7.0.1      |
| Application         | `0x31000`–           | this firmware (`nrf52840_xxaa.hex`)   |
| Bootloader          | `0xF4000`–`0xFCC08`  | Nordic Secure USB-DFU bootloader      |
| MBR params page     | `0xFE000`            | MBR/bootloader shared params          |
| Bootloader settings | `0xFF000`–`0xFF324`  | app validation CRC (boot decision)    |

UICR pointers (written from the bootloader hex, cleared by a chip erase):
- `NRF_UICR->NRFFW[0]` (`0x10001014`) = bootloader start = `0xF4000`
- `NRF_UICR->NRFFW[1]` (`0x10001018`) = MBR params page = `0xFE000`

Boot chain: **MBR → (UICR NRFFW[0]) bootloader → validate app via settings-page CRC → app** (or stay in DFU if invalid / requested).

---

## 2. Hardware setup — Pico as CMSIS-DAP SWD probe

Flash the Pico once with the CMSIS-DAP **debugprobe** firmware, then wire it to the XIAO:

| Pico pin | XIAO pad |
|----------|----------|
| GP2      | SWCLK    |
| GP3      | SWDIO    |
| GND      | GND      |

> **Do NOT wire Pico 3V3 to the XIAO** if the XIAO is self-powered over its own
> USB-C. Power both from USB independently.

Two USB cables go to the host: one to the **Pico** (the SWD probe) and one to the
**XIAO** (its native USB, used for the app console and USB-DFU).

Verify the probe is seen:
```sh
pyocd list          # should show a CMSIS-DAP probe (Raspberry Pi - Debugprobe)
```

---

## 3. Paths used below

External artifacts live outside this repo:
```sh
S340_HEX=/path/to/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex
BOOTLOADER_HEX=/path/to/bootloader_usb_s340.hex
```
In-repo build outputs (after a `make` build):
```sh
APP_HEX=firmware/_build/nrf52840_xxaa.hex
SETTINGS_HEX=firmware/_build/settings.hex
DFU_ZIP=firmware/_build/app_dfu.zip
```

---

## 4. First-time provisioning (full board, from blank)

This puts down **everything** needed so the board is bootable AND updatable over
USB: SoftDevice + app + bootloader + settings, in one chip-erase pass.

### 4a. Build the app
```sh
make -C firmware -j8 \
  GNU_INSTALL_ROOT=/path/to/arm-none-eabi-gcc/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/path/to/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/path/to/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include \
  TESTBOARD=1        # TESTBOARD=1 adds the OLED/LED/buzzer/button test aid; 0 for production
```

### 4b. Generate the bootloader settings page
The settings page tells the bootloader the app is valid (CRC) so it boots it
instead of sitting in DFU. **`--no-backup` is required** — the default backup
address (`settings − 0x1000 = 0xFE000`) collides with the MBR params page.
```sh
make -C firmware settings          # wraps the nrfutil settings command below
# equivalently:
nrfutil settings generate --family NRF52840 \
  --application firmware/_build/nrf52840_xxaa.hex \
  --application-version 1 --bootloader-version 1 \
  --bl-settings-version 2 --no-backup \
  firmware/_build/settings.hex
```

### 4c. Flash all four images in one chip-erase pass
A single `--erase chip` (ERASEALL) clears flash **and UICR**; programming the
bootloader hex re-writes the UICR NRFFW pointers.
```sh
make -C firmware flash-full        # wraps the pyocd command below
# equivalently:
pyocd flash -t nrf52840 --erase chip \
  "$S340_HEX" \
  firmware/_build/nrf52840_xxaa.hex \
  "$BOOTLOADER_HEX" \
  firmware/_build/settings.hex
pyocd reset -t nrf52840
```

After reset the board enumerates as **"Garmin Treadmill Bridge"** — proof the
bootloader validated and booted the app.

### 4d. (optional) Verify UICR
```sh
pyocd commander -t nrf52840 -c "halt" \
  -c "read32 0x10001014" -c "read32 0x10001018" -c "go"
# expect: 0x000f4000  and  0x000fe000
```

---

## 5. Fast app-only reflash (SWD) — ⚠ BROKEN, use `flash-full`

> **⚠ `flash-app` does not work on this setup. Use `make flash-full` after any
> app change.**
>
> It reproducibly leaves the board parked in the bootloader at
> `pc = 0x000f8308` with the app never started (`.bss` not zeroed,
> `s_heartbeat_cnt` garbage). Reproduced three times on 2026-07-29, twice
> deliberately, with both a growing and a shrinking image. `flash-full` has never
> failed.
>
> An earlier version of this note claimed `flash-app` "boots it reliably" because
> it carries the settings page. That was wrong — carrying the settings page is
> necessary but not sufficient. Full measured detail and the one outstanding
> measurement that would likely settle it are in `docs/HANDOFF.md` under
> "SWD / flashing recipes".

The intent was: when only the app changed and the SoftDevice/bootloader are
already in place, skip the chip erase and sector-erase just the app region.
The target still exists and now erases the settings sector explicitly (which did
fix a genuine skipped-write), but it is **not** reliable:

```sh
make -C firmware flash-app         # ⚠ prints a warning and a verify reminder
```

Whatever you flash, **always confirm the app actually started** rather than
assuming success:

```sh
HB=$(nm -S firmware/_build/nrf52840_xxaa.out | grep ' s_heartbeat_cnt' | awk '{print "0x"$1}')
pyocd commander -t nrf52840 -O connect_mode=attach -c "read32 $HB"
```

A small monotonically increasing value means the app booted. Garbage means
`.bss` was never zeroed and the bootloader kept control — run `flash-full`.

---

## 6. Update over USB (Secure DFU) — no SWD needed

This is the "updatable over USB" path. It transfers a **signed** package to the
Nordic bootloader over a USB-serial link; the bootloader verifies the ECDSA-P256
signature + CRC, writes the app bank, refreshes the settings page, and reboots.

### 6a. Build the signed package
```sh
make -C firmware dfu               # produces firmware/_build/app_dfu.zip
# runs: nrfutil pkg generate --hw-version 52 --sd-req 0xCE \
#         --application _build/nrf52840_xxaa.hex --application-version 1 \
#         --key-file ../dfu/dfu_private_key.pem  _build/app_dfu.zip
```
- `--sd-req 0xCE` is the S340 v7.0.1 firmware ID. `--hw-version 52` = nRF52840.
- The signing key is `dfu/dfu_private_key.pem` (git-ignored secret; the matching
  public key is compiled into the bootloader as `dfu/dfu_public_key.c`).

### 6b. Put the running app into DFU mode
Write the DFU-start magic `0xB1` to `GPREGRET` (`POWER->GPREGRET @ 0x4000051C`)
and do a **software** reset (SYSRESETREQ preserves GPREGRET; a power-on/pin reset
would clear it). Requires the SWD probe attached:
```sh
make -C firmware dfu-enter         # wraps the pyocd command below
# equivalently:
pyocd commander -t nrf52840 -c "halt" \
  -c "write32 0x4000051C 0xB1" -c "reset -t sw"
```
The board re-enumerates as **"Secure DFU Bootloader"** with its own USB-serial node.

> No SWD probe? The bootloader also enters DFU automatically when the app is
> invalid/absent. A user-facing "enter DFU" trigger from the app (e.g. a console
> command or button that sets GPREGRET then resets) is a future nicety — not yet
> wired up.

### 6c. Find the bootloader's serial port and flash
```sh
# Identify the nRF's USB-serial node (NOT the Pico's — see §7):
ioreg -r -c IOSerialBSDClient -w 0 | grep -A1 -i callout

make -C firmware flash-dfu SERIAL=/dev/cu.usbmodemXXXXXXXX
# runs: nrfutil dfu usb-serial -pkg _build/app_dfu.zip -p $(SERIAL)
```
Success prints **`Device programmed.`** and the board reboots into the app,
re-enumerating as "Garmin Treadmill Bridge".

---

## 7. USB-CDC console (logs + interactive control)

The app exposes a CDC-ACM virtual serial port: it streams the `alive N` heartbeat
and accepts the uppercase ctrl grammar (`SPEED`, `SCAN`, `CONNECT`, `STOP`,
`LIST`, `STATUS`), replying with JSON.

```sh
screen /dev/cu.usbmodemXXXXXXXX 115200      # or: picocom, minicom, pyserial
# STATUS -> {"cmd":"status","connected":false}
# LIST   -> {"cmd":"list","devices":[]}
# SCAN   -> {"cmd":"scan","ok":true}
```

### Picking the right port (important gotcha)
With the Pico probe attached there are **two** CDC devices. The Pico debugprobe
firmware exposes CMSIS-DAP **plus its own UART bridge**, so `/dev/cu.usbmodemNNN`
is often the **Pico**, not the app. The app's node is keyed to the nRF52 chip
serial (the 12-hex-digit chip serial + interface-index suffix `1`), e.g.
`/dev/cu.usbmodem<CHIPSERIAL>1`.

Map nodes to devices by USB serial number rather than trusting the number:
```sh
ioreg -r -c IOSerialBSDClient -w 0 | grep IOCalloutDevice
ioreg -p IOUSB -l -w 0 | grep -E '"USB Product Name"|"USB Serial Number"'
# The app is "Garmin Treadmill Bridge" (VID 0x1915 / PID 0x521F).
```

---

## 8. Troubleshooting

- **`pyocd` hangs on "Waiting for a debug probe to be connected…"** — intermittent
  connect latency; retry. Check the Pico wiring and that `pyocd list` sees it.
- **Console enumerates but zero data / no reply** — you're likely on the Pico's
  UART node. See §7 and select the "Garmin Treadmill Bridge" serial.
- **Enumerates but interfaces don't bind (`!matched`, no TTY)** — historically a
  **cable/port** problem (marginal USB-C cable or host controller), not firmware:
  Nordic's own bootloader failed identically until we swapped cable/port. Try a
  known-good data cable and a direct port.
- **App won't boot after a flash** (`pc` in `0x000F4000+`, `s_heartbeat_cnt`
  garbage) — the settings-page CRC does not match the app, so the bootloader
  fails boot validation and keeps control. **Run `make flash-full`** (§4); it is
  the only target that reliably fixes this. Do **not** reach for `flash-app` —
  it is the usual *cause* (see §5). The USB-DFU path (§6) also refreshes the
  settings page. Confirm recovery with the heartbeat check in §5, not by
  assuming the flash worked.
- **DFU rejects the package** — check `--sd-req 0xCE` (S340 v7.0.1) and that the
  signing key matches the bootloader's embedded public key.
- **Reading RAM/RTT over SWD requires a halt**, which breaks live USB enumeration.
  Do quick `halt … go` reads; don't hold the core halted while testing USB.
