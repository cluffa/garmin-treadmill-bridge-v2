# Environment setup from scratch

Everything needed to go from a blank Mac to `make host-test`, `make firmware`,
`make bootloader`, `make flash-full`, and `make ciq-build`. Section numbers match
the hints printed by **`make doctor`**, which checks every item below.

> **Verified 2026-11-15** on macOS (Apple Silicon) from a fresh `git clone`, a
> freshly unzipped SDK, and freshly installed toolchains in an isolated
> directory: host tests, firmware, DFU package, settings page, CIQ build, and
> `pace-test` all passed, and `make bootloader` reproduced the provisioned
> `bootloader_usb_s340.hex` **byte-for-byte**. Linux should work with the same
> versions but has not been tried.

## At a glance

| # | Dependency | Version (exact) | Source | Default location | Needed by |
|---|------------|-----------------|--------|------------------|-----------|
| 1 | Host C compiler, python3, uv | any recent | Xcode CLT, astral.sh | `PATH` | `host-test`, `pace-test`, mocks |
| 2 | nRF5 SDK | **17.1.0 (ddde560)**, unmodified | Nordic | `~/nRF5_SDK_17.1.0_ddde560` | `firmware`, `bootloader` |
| 3 | arm-none-eabi-gcc (firmware) | **7.2.1** (7-2017-q4-major) | PlatformIO | `~/.platformio/packages/toolchain-gccarmnoneeabi/bin/` | `firmware` |
| 4 | S340 SoftDevice + API headers | **7.0.1** | thisisant.com (login, not redistributable) | `$NRF_DEPS/ANT_s340_nrf52_7.0.1/` | `firmware`, `flash-*` |
| 5 | arm-none-eabi-gcc (bootloader) | **10.3-2021.10** | Arm | `~/gcc-arm-none-eabi-10.3-2021.10/bin/` | `bootloader` only |
| 6 | ANT network key, ANT license, DFU key pair | — | thisisant.com / `nrfutil` | git-ignored files in repo | `firmware` (runtime), `dfu` |
| 7 | pyOCD, nrfutil + nrf5sdk-tools, Pico debugprobe | pyocd 0.45.1, nrfutil 8.2.0 | uv, Nordic, Raspberry Pi | `PATH` | `flash-*`, `dfu`, `settings` |
| 8 | Connect IQ SDK, device files, developer key, libmtp | SDK **9.2.0**, `fenix8solar51mm` | Garmin, Homebrew | `~/Library/Application Support/Garmin/ConnectIQ/` | `ciq-build`, `sideload` |

`NRF_DEPS` defaults to `~/workspace/nrf52`. Every path above is a `?=` default
in the Makefiles, so any of them can be overridden on the command line, e.g.
`make firmware SDK_ROOT=/opt/nrf5 NRF_DEPS=/opt/nrf-deps`. `make doctor` accepts
the same overrides and checks the paths a build would really use.

---

## 1. Host tools

```sh
xcode-select --install                              # cc, make
brew install python                                 # python3 (any 3.10+)
curl -LsSf https://astral.sh/uv/install.sh | sh     # uv: runs pace_lag_report.py, mock_*.py
```

That is all `make host-test` needs (`core/` is pure C; `test/mock` builds a
`.so` with `cc` and runs stdlib-only Python). `make pace-test` and the BLE mocks
are `uv run --script` files that declare their own deps (`garmin-fit-sdk`,
`bless==0.3.0`), so uv fetches them on first run. `make pace-test` also needs a
recorded `.FIT` in `test/` — those are untracked on purpose (see CLAUDE.md).

## 2. nRF5 SDK 17.1.0

```sh
cd ~
curl -fLO https://nsscprodmedia.blob.core.windows.net/prod/software-and-other-downloads/sdks/nrf5/binaries/nrf5_sdk_17.1.0_ddde560.zip
shasum -a 256 nrf5_sdk_17.1.0_ddde560.zip
# 5bfe38e744c39fd7f30e10077ba12df306ef91f368894795d6a3e7a62dc68061
unzip -q nrf5_sdk_17.1.0_ddde560.zip            # -> ~/nRF5_SDK_17.1.0_ddde560
```

**Leave the SDK unmodified.** The firmware build passes `GNU_INSTALL_ROOT`
explicitly, so `components/toolchain/gcc/Makefile.posix` does not need editing.
The one file the bootloader needs changed (`examples/dfu/dfu_public_key.c`) is
installed by `make bootloader` itself, which keeps the original as `.orig`.
(The SDK on the original dev machine differs from the zip in exactly those two
files — `Makefile.posix` and `dfu_public_key.c` — and nothing else.)

## 3. Firmware compiler: GCC 7.2.1 via PlatformIO

```sh
brew install platformio
pio pkg install --global --tool "platformio/toolchain-gccarmnoneeabi@1.70201.0"
~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-gcc --version
# arm-none-eabi-gcc (GNU Tools for Arm Embedded Processors 7-2017-q4-major) 7.2.1 20170904
```

Use this exact compiler for the app firmware. **Not** Homebrew's
`arm-none-eabi-gcc` (16.x): nRF5 SDK 17.1.0 does not build cleanly with it.
`make doctor` fails if the version is not 7.2.1. PlatformIO package `1.70201.0`
is GCC 7.2.1; the package is a native arm64/x86_64 macOS build.

## 4. S340 SoftDevice 7.0.1 (ANT + BLE)

S340 is **not** in the nRF5 SDK and is **not redistributable** — never commit it.

1. Create a free account at <https://www.thisisant.com> and sign in.
2. Download **"ANT S340 SoftDevice v7.0.1 for nRF52"** (the zip contains
   `ANT_s340_nrf52_7.0.1.hex` and `ANT_s340_nrf52_7.0.1.API/`).
3. Unzip so this path exists:

```sh
mkdir -p ~/workspace/nrf52            # = $NRF_DEPS
unzip ANT_s340_nrf52_7.0.1.zip -d ~/workspace/nrf52
ls ~/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex \
   ~/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include/ant_interface.h
```

`S340_API` (headers) is needed to compile; `S340_HEX` is needed only to flash.
`make dfu` hard-codes `--sd-req 0xCE`, which is S340 7.0.1's FWID — a different
SoftDevice version needs that changed too. (An older `ANT_s340_nrf52840_6.1.1`
download may also be lying around on the dev machine; it is **not** used.)

## 5. USB-DFU bootloader

`make flash-full` writes `$(NRF_DEPS)/build/bootloader_usb_s340.hex`. It is the
**stock** SDK project `examples/dfu/secure_bootloader/pca10056_usb` (no
SoftDevice, CC310 crypto, USB CDC transport) with this repo's
`dfu/dfu_public_key.c` compiled in — nothing else changed. Because it is the
`_nosd` variant it does not care which SoftDevice is on the chip; "s340" in the
filename is only a label.

It was built with **Arm GCC 10.3-2021.10**, not the firmware's 7.2.1, and that
is the toolchain that reproduces the provisioned hex byte-for-byte:

```sh
softwareupdate --install-rosetta --agree-to-license   # the 10.3 mac build is x86_64
cd ~
curl -fLO https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-mac.tar.bz2
md5 gcc-arm-none-eabi-10.3-2021.10-mac.tar.bz2    # 7f2a7b7b23797302a9d6182c6e482449
tar -xjf gcc-arm-none-eabi-10.3-2021.10-mac.tar.bz2  # -> ~/gcc-arm-none-eabi-10.3-2021.10

cd <repo>
make bootloader          # tools/build_bootloader.sh
# ok — bootloader written to ~/workspace/nrf52/build/bootloader_usb_s340.hex
```

Overrides: `SDK_ROOT`, `NRF_DEPS`, `BOOTLOADER_HEX`, `BL_GNU_INSTALL_ROOT`,
`BL_GNU_VERSION` (environment variables for the script).

**You only need this for first-time provisioning** (`make flash-full`) or after
changing the DFU key. Everyday updates go over USB-DFU to the bootloader that is
already on the board.

## 6. Keys and licenses (git-ignored, never committed)

```sh
cp firmware/ant_network_key.h.example firmware/ant_network_key.h
cp firmware/ant_license.mk.example   firmware/ant_license.mk
```

| File | Get it from | If missing / placeholder |
|------|-------------|--------------------------|
| `firmware/ant_network_key.h` | thisisant.com → ANT+ network key (free for personal use, requires agreeing to the ANT+ adopter terms) | Missing: build fails (`fatal error: ant_network_key.h`). All-zero template: **builds, but no watch pairs** — `make doctor` warns about it. |
| `firmware/ant_license.mk` | thisisant.com → S340 evaluation license key (16-byte hex) | Build warns; `sd_ant_enable()` fails at runtime. |
| `dfu/dfu_private_key.pem` | Your existing key (back it up — see below), or `nrfutil keys generate` | `make dfu` refuses to run. |

**DFU key pair — read before generating a new one.** A provisioned board only
accepts packages signed by the private key matching the public key compiled
into *its* bootloader (`dfu/dfu_public_key.c`, committed). Keep
`dfu/dfu_private_key.pem` backed up somewhere private (it is *not* in git). If
it is lost, the board can no longer be updated over USB; recovery is:

```sh
nrfutil keys generate dfu/dfu_private_key.pem
nrfutil keys display --key pk --format code dfu/dfu_private_key.pem > dfu/dfu_public_key.c
make bootloader && make firmware && make flash-full    # SWD probe required
```

## 7. Flashing tools

```sh
uv tool install pyocd==0.45.1          # SWD via the Pico probe (NOT nrfjprog/J-Link)
pyocd --version
```

**nrfutil** (Nordic's v7+ single binary; the legacy `pip install nrfutil` 6.x is
*not* what the Makefile calls):

1. Download the macOS binary from
   <https://www.nordicsemi.com/Products/Development-tools/nRF-Util>.
2. Install it and the nRF5 SDK command set:

```sh
install -m 755 ~/Downloads/nrfutil ~/.local/bin/nrfutil   # any dir on PATH
nrfutil install nrf5sdk-tools          # provides: pkg, settings, keys, dfu
nrfutil list                           # nrf5sdk-tools 1.1.0 (verified with nrfutil 8.2.0)
```

`mergehex`/`nrfjprog` are **not** used by this repo.

**SWD probe** (first-time provisioning only): a Raspberry Pi Pico running the
CMSIS-DAP *debugprobe* firmware. Download `debugprobe_on_pico.uf2` from
<https://github.com/raspberrypi/debugprobe/releases>, hold BOOTSEL while plugging
in the Pico, and copy the `.uf2` onto the `RPI-RP2` drive. Wiring and the full
provisioning procedure: `docs/flashing.md` §2 and §4. `pyocd list` should then
show `Raspberry Pi - Debugprobe`.

## 8. Watch side (Connect IQ)

1. Install the **Connect IQ SDK Manager** from
   <https://developer.garmin.com/connect-iq/sdk/>, sign in with a Garmin account,
   and use it to install SDK **9.2.0** (`connectiq-sdk-mac-9.2.0-2026-06-09-92a1605b2`)
   and the **fenix 8 Solar 51mm** device (`fenix8solar51mm`). Both land under
   `~/Library/Application Support/Garmin/ConnectIQ/`. A different SDK build
   works too; pass `CIQ_SDK=<path>` to `make ciq-build`.
2. Create a developer key (any RSA-4096 key; it signs sideloaded builds):

```sh
openssl genrsa -out ~/Documents/garmin_developer_key.pem 4096
openssl pkcs8 -topk8 -inform PEM -outform DER -nocrypt \
  -in ~/Documents/garmin_developer_key.pem -out ~/Documents/garmin_developer_key.der
```

3. Sideloading uses MTP (the fenix 8 has no USB mass storage):

```sh
brew install libmtp
```

Then `make ciq-build && make sideload` — details and gotchas in `watch/README.md`.

## 9. Verify

```sh
make doctor        # every line ok (warn/MISS lines name the section above)
make host-test     # all core/ + mock suites OK
make firmware      # -> firmware/_build/nrf52840_xxaa.hex
make bootloader    # only if provisioning a board
make dfu settings  # needs dfu/dfu_private_key.pem
make ciq-build     # -> watch/garmin_data_field/out/app.prg
```

Then flash per `docs/flashing.md`.
