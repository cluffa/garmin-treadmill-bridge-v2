# DFU (Device Firmware Update) via USB

## One-time bootloader setup

The XIAO nRF52840 ships with either an Adafruit UF2/USB bootloader or a Nordic secure
bootloader. DFU packages signed with a private key will only be accepted by a Nordic
secure bootloader that has the matching **public key** compiled in.

### Option A: Nordic secure bootloader (recommended for signed DFU)

1. Build or obtain a Nordic `secure_bootloader` hex that embeds `dfu_public_key.c`.
2. Flash it once via SWD:
   ```
   nrfjprog -f nrf52 --program secure_bootloader_xxaa.hex --sectorerase
   nrfjprog -f nrf52 --reset
   ```

If you compiled the bootloader yourself, ensure `dfu_public_key.c` (the one in this
directory) was included in the build.

### Option B: Adafruit UF2 bootloader (stock XIAO)

The Adafruit bootloader does **not** support signed DFU packages. You must either
replace it with a Nordic secure bootloader (Option A), or use the SWD fallback:
```
make -C firmware flash-app
```

## Key management

| File                                | Purpose                                    | Committed? |
|-------------------------------------|--------------------------------------------|------------|
| `dfu_public_key.c`                  | Public key compiled into the bootloader    | Yes        |
| `dfu_private_key.pem`               | Private key for signing DFU packages       | **No**     |
| `dfu_private_key.pem.example`       | Placeholder showing the expected format    | Yes        |

Generate a new keypair:
```
nrfutil keys generate dfu_private_key.pem
nrfutil keys display --key pk --format code dfu_private_key.pem > dfu_public_key.c
```

The `dfu_private_key.pem` is git-ignored (see `.gitignore`). For local builds, symlink
or copy your real private key into this directory so `make dfu` can find it.

## Everyday DFU loop

### 1. Build the firmware

```
make -C firmware -j8 \
  GNU_INSTALL_ROOT=... GNU_VERSION=7.2.1 SDK_ROOT=... S340_API=...
```

### 2. Create a signed DFU package

```
make -C firmware dfu
```

This runs `nrfutil pkg generate` with `--hw-version 52 --sd-req 0xCE` (S340 v7.0.1)
and writes `firmware/_build/app_dfu.zip`.

### 3. Flash over USB

Put the device in DFU mode (bootloader), then:

```
make -C firmware flash-dfu SERIAL=/dev/cu.usbmodemXXXX
```

### SWD fallback (if the bootloader is missing or misconfigured)

```
make -C firmware flash-sd     # one-time SoftDevice flash
make -C firmware flash-app    # flash app via nrfjprog
```

These targets use `nrfjprog` over a J-Link or similar SWD probe.
