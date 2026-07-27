# DFU (Device Firmware Update) via USB

The XIAO nRF52840 has **no onboard debugger**, so all flashing goes through either:

- **SWD:** a Raspberry Pi Pico running CMSIS-DAP firmware, driven by **pyOCD**.
- **USB:** the Nordic Secure USB-DFU bootloader, driven by **nrfutil**.

The authoritative flashing guide — hardware wiring, full provisioning, everyday
loops, and troubleshooting — is in **`docs/flashing.md`**. This file covers the
DFU-specific key setup and package creation steps only.

## One-time bootloader setup

The XIAO nRF52840 ships with an Adafruit UF2/USB bootloader. DFU packages
signed with a private key will only be accepted by a Nordic secure bootloader
that has the matching **public key** compiled in. The bootloader is flashed once
over SWD alongside the SoftDevice (see `docs/flashing.md` §4 for the
`make flash-full` procedure, which does this in one chip-erase pass).

If you compiled the bootloader yourself, ensure `dfu_public_key.c` (the one in
this directory) was included in the build.

## Key management

| File                          | Purpose                                 | Committed? |
|-------------------------------|-----------------------------------------|------------|
| `dfu_public_key.c`            | Public key compiled into the bootloader | Yes        |
| `dfu_private_key.pem`         | Private key for signing DFU packages    | **No**     |
| `dfu_private_key.pem.example` | Placeholder showing the expected format | Yes        |

Generate a new keypair:
```
nrfutil keys generate dfu_private_key.pem
nrfutil keys display --key pk --format code dfu_private_key.pem > dfu_public_key.c
```

The `dfu_private_key.pem` is git-ignored (see `.gitignore`). For local builds,
symlink or copy your real private key into this directory so `make dfu` can
find it.

## Everyday DFU loop

Build the firmware first (see `CLAUDE.md` or `docs/flashing.md` §4a), then:

```sh
# Create a signed DFU package:
make dfu            # produces firmware/_build/app_dfu.zip

# Put the running app into DFU mode (requires SWD probe):
make dfu-enter

# Push the package over USB:
make flash-dfu SERIAL=/dev/cu.usbmodemXXXX
```

`make dfu` runs `nrfutil pkg generate` with `--hw-version 52 --sd-req 0xCE`
(S340 v7.0.1). The signing key is `dfu/dfu_private_key.pem` (git-ignored; the
matching public key in `dfu/dfu_public_key.c` is compiled into the bootloader).

All three DFU targets (`dfu`, `dfu-enter`, `flash-dfu`) **consume an existing
build** — they do not rebuild. Build first, then package/flash.

## SWD fallback (if the bootloader is missing or misconfigured)

```sh
make flash-full      # first-time: SOD+app+bootloader+settings, chip erase
make flash-app       # fast app-only reflash (+ settings page)
```

These are SWD targets that require the Pico CMSIS-DAP probe and **pyOCD** (not
nrfjprog / J-Link). Full details in `docs/flashing.md`.
