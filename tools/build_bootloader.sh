#!/usr/bin/env bash
# Build the Nordic Secure USB-DFU bootloader that `make flash-full` writes.
#
# This is the stock nRF5 SDK 17.1.0 `examples/dfu/secure_bootloader/pca10056_usb`
# project, unmodified, with ONE change: the SDK's examples/dfu/dfu_public_key.c
# is replaced by this repo's dfu/dfu_public_key.c, so the bootloader accepts
# packages signed with dfu/dfu_private_key.pem. pca10056_usb is the
# no-SoftDevice variant (CC310 crypto, nrf_soc_nosd), so it is SoftDevice
# agnostic and boots S340 fine — the "_s340" in the output name is only a label.
#
# Verified 2026-11-15: a pristine SDK zip + this key + Arm GCC 10.3-2021.10
# reproduces the provisioned bootloader_usb_s340.hex byte-for-byte.
# See docs/setup.md §5.
#
# Usage:
#   tools/build_bootloader.sh                       # defaults below
#   SDK_ROOT=... BL_GNU_INSTALL_ROOT=... BOOTLOADER_HEX=... tools/build_bootloader.sh
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
SDK_ROOT=${SDK_ROOT:-$HOME/nRF5_SDK_17.1.0_ddde560}
BL_GNU_INSTALL_ROOT=${BL_GNU_INSTALL_ROOT:-$HOME/gcc-arm-none-eabi-10.3-2021.10/bin/}
BL_GNU_VERSION=${BL_GNU_VERSION:-10.3.1}
NRF_DEPS=${NRF_DEPS:-$HOME/workspace/nrf52}
BOOTLOADER_HEX=${BOOTLOADER_HEX:-$NRF_DEPS/build/bootloader_usb_s340.hex}

PROJ=$SDK_ROOT/examples/dfu/secure_bootloader/pca10056_usb/armgcc
KEY_DST=$SDK_ROOT/examples/dfu/dfu_public_key.c
KEY_SRC=$REPO/dfu/dfu_public_key.c

die() { echo "error: $*" >&2; exit 1; }
[ -f "$PROJ/Makefile" ] || die "no bootloader project at $PROJ — set SDK_ROOT (docs/setup.md §2)"
[ -x "${BL_GNU_INSTALL_ROOT%/}/arm-none-eabi-gcc" ] || die "no arm-none-eabi-gcc in $BL_GNU_INSTALL_ROOT — set BL_GNU_INSTALL_ROOT (docs/setup.md §5)"
[ -f "$KEY_SRC" ] || die "missing $KEY_SRC"

# Install the public key into the SDK, keeping the SDK's original once.
if ! cmp -s "$KEY_SRC" "$KEY_DST"; then
  [ -f "$KEY_DST.orig" ] || cp "$KEY_DST" "$KEY_DST.orig"
  cp "$KEY_SRC" "$KEY_DST"
  echo "installed dfu/dfu_public_key.c into the SDK (original kept as .orig)"
fi

make -C "$PROJ" -j8 GNU_INSTALL_ROOT="${BL_GNU_INSTALL_ROOT%/}/" GNU_VERSION="$BL_GNU_VERSION"

mkdir -p "$(dirname "$BOOTLOADER_HEX")"
cp "$PROJ/_build/nrf52840_xxaa.hex" "$BOOTLOADER_HEX"
echo "ok — bootloader written to $BOOTLOADER_HEX"
