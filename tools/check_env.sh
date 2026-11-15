#!/usr/bin/env bash
# `make doctor`: report which parts of the build chain are present.
# Every check names the docs/setup.md section that installs the missing piece.
# Exit status is non-zero only if something needed by `make host-test` or
# `make firmware` is missing; hardware/watch tools are reported but optional.
#
# Paths come from the same variables the Makefiles use, so passing overrides
# (`make doctor SDK_ROOT=/elsewhere`) checks what a build would actually use.
set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
: "${GNU_INSTALL_ROOT:=$HOME/.platformio/packages/toolchain-gccarmnoneeabi/bin/}"
: "${SDK_ROOT:=$HOME/nRF5_SDK_17.1.0_ddde560}"
: "${NRF_DEPS:=$HOME/workspace/nrf52}"
: "${S340_API:=$NRF_DEPS/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include}"
: "${S340_HEX:=$NRF_DEPS/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex}"
: "${BOOTLOADER_HEX:=$NRF_DEPS/build/bootloader_usb_s340.hex}"
: "${BL_GNU_INSTALL_ROOT:=$HOME/gcc-arm-none-eabi-10.3-2021.10/bin/}"
: "${CIQ_SDK:=$HOME/Library/Application Support/Garmin/ConnectIQ/Sdks/connectiq-sdk-mac-9.2.0-2026-06-09-92a1605b2}"
: "${CIQ_KEY:=$HOME/Documents/garmin_developer_key.der}"
: "${DFU_KEY:=$REPO/dfu/dfu_private_key.pem}"

fail=0
ok()   { printf '  \033[32mok\033[0m    %-34s %s\n' "$1" "$2"; }
miss() { printf '  \033[31mMISS\033[0m  %-34s %s\n' "$1" "$2"; [ "$3" = req ] && fail=1; }
warn() { printf '  \033[33mwarn\033[0m  %-34s %s\n' "$1" "$2"; }
chk_cmd()  { if command -v "$2" >/dev/null 2>&1; then ok "$1" "$(command -v "$2")"; else miss "$1" "not on PATH — setup.md $3" "$4"; fi; }
chk_path() { if [ -e "$2" ]; then ok "$1" "$2"; else miss "$1" "$2 — setup.md $3" "$4"; fi; }

echo "Host tests (make host-test):"
chk_cmd  "C compiler (cc)"            cc       "§1" req
chk_cmd  "python3"                    python3  "§1" req
chk_cmd  "uv (pace-test, mocks)"      uv       "§1" opt

echo "Firmware (make firmware):"
GCC="${GNU_INSTALL_ROOT%/}/arm-none-eabi-gcc"
if [ -x "$GCC" ]; then
  v=$("$GCC" -dumpversion)
  if [ "$v" = 7.2.1 ]; then ok "arm-none-eabi-gcc 7.2.1" "$GCC"; else miss "arm-none-eabi-gcc 7.2.1" "found $v at $GCC — must be 7.2.1, setup.md §3" req; fi
else miss "arm-none-eabi-gcc 7.2.1" "$GCC — setup.md §3" req; fi
chk_path "nRF5 SDK 17.1.0"            "$SDK_ROOT/components/toolchain/gcc/Makefile.common" "§2" req
chk_path "S340 7.0.1 API headers"     "$S340_API/ant_interface.h" "§4" req
chk_path "ANT network key"            "$REPO/firmware/ant_network_key.h" "§6" req
if [ -f "$REPO/firmware/ant_network_key.h" ] && grep -qE '0x00, *0x00, *0x00, *0x00, *0x00, *0x00, *0x00, *0x00' "$REPO/firmware/ant_network_key.h"; then
  warn "ANT network key" "still the all-zero template — builds, but no watch will pair (setup.md §6)"
fi
chk_path "ANT license (ant_license.mk)" "$REPO/firmware/ant_license.mk" "§6" opt

echo "Flash / DFU (make flash-full, dfu, settings):"
chk_path "S340 7.0.1 hex"             "$S340_HEX" "§4" opt
chk_path "bootloader hex"             "$BOOTLOADER_HEX" "§5 (tools/build_bootloader.sh)" opt
chk_path "GCC 10.3 (bootloader only)" "${BL_GNU_INSTALL_ROOT%/}/arm-none-eabi-gcc" "§5" opt
chk_cmd  "pyocd"                      pyocd    "§7" opt
if command -v nrfutil >/dev/null 2>&1; then
  if nrfutil list 2>/dev/null | grep -q nrf5sdk-tools; then ok "nrfutil + nrf5sdk-tools" "$(command -v nrfutil)"
  else miss "nrfutil + nrf5sdk-tools" "run: nrfutil install nrf5sdk-tools — setup.md §7" opt; fi
else miss "nrfutil + nrf5sdk-tools" "not on PATH — setup.md §7" opt; fi
chk_path "DFU signing key"            "$DFU_KEY" "§6" opt

echo "Watch (make ciq-build, sideload):"
chk_path "Connect IQ SDK (monkeyc)"   "$CIQ_SDK/bin/monkeyc" "§8" opt
chk_path "CIQ developer key"          "$CIQ_KEY" "§8" opt
chk_cmd  "libmtp (mtp-sendfile)"      mtp-sendfile "§8" opt

echo
if [ $fail = 0 ]; then echo "host-test and firmware prerequisites present."; else echo "missing required pieces — see docs/setup.md"; fi
exit $fail
