# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
#
# Every path below is overridable: `make SDK_ROOT=/elsewhere firmware` works,
# because these are ?= defaults that a command-line assignment beats, and the
# resolved values are what get forwarded to the sub-make. (Passing a literal
# on the sub-make command line, as this file used to, silently outranks
# anything the user sets.)

.PHONY: host-test firmware dfu settings flash-dfu flash-full flash-sd flash-app \
        dfu-enter reset clean help mock-bridge mock-test sideload usb-kick

GNU_INSTALL_ROOT ?= /Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/
GNU_VERSION      ?= 7.2.1
SDK_ROOT         ?= $(HOME)/nRF5_SDK_17.1.0_ddde560
S340_API         ?= $(HOME)/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include

FW_MAKE_ARGS = \
  GNU_INSTALL_ROOT=$(GNU_INSTALL_ROOT) \
  GNU_VERSION=$(GNU_VERSION) \
  SDK_ROOT=$(SDK_ROOT) \
  S340_API=$(S340_API)

help:
	@echo "Build:"
	@echo "  make host-test              core/ unit tests (no toolchain needed)"
	@echo "  make check-uuid             assert firmware/mock/watch agree on the A6ED UUID"
	@echo "  make mock-test              mock-bridge unit tests (decoder + probe ABI)"
	@echo "  make mock-bridge            run the macOS mock bridge (debug the watch data field)"
	@echo "  make firmware               build the nRF52840 image"
	@echo "Watch (Connect IQ) — see watch/README.md:"
	@echo "  make sideload               push the built .prg to the watch over MTP (watch plugged in)"
	@echo "  watch/garmin_data_field     writes workout targets to A6ED0004"
	@echo "  watch/garmin_ctrl_app       SCAN/CONNECT picker UI (A6ED0002/0003)"
	@echo "  make usb-kick               make the app's USB console appear (needed after EVERY app boot)"
	@echo "Package / flash (see docs/flashing.md — these consume an existing build):"
	@echo "  make dfu                    signed USB-DFU package"
	@echo "  make settings               bootloader settings page"
	@echo "  make flash-full             first-time SWD provisioning (SD+app+BL+settings)"
	@echo "  make flash-app              fast app-only SWD reflash (+ settings)"
	@echo "  make dfu-enter              kick a running app into DFU over SWD"
	@echo "  make flash-dfu SERIAL=/dev/cu.usbmodemXXXX   push a package over USB"
	@echo "  make reset                  reset the target"
	@echo ""
	@echo "First build on a new machine: copy firmware/ant_network_key.h.example"
	@echo "and firmware/ant_license.mk.example (see README)."

host-test: check-uuid
	$(MAKE) -C test/host
	$(MAKE) -C test/mock test

# The watch discovers the bridge by filtering on the 128-bit A6ED service UUID,
# so firmware/mock/CIQ disagreement is SILENT — the watch just never finds the
# device. Cheap to check, so it runs as part of the standard gate.
check-uuid:
	@python3 test/check_uuid_contract.py

# Mock bridge: a macOS BLE peripheral that impersonates this firmware so the
# Garmin data field can be debugged without the hardware in the loop.
# See docs/superpowers/specs/2026-07-30-mock-bridge-design.md.
mock-test:
	$(MAKE) -C test/mock test

mock-bridge:
	$(MAKE) -C test/mock
	cd test/mock && ./mock_bridge.py

# Sideload a built Connect IQ app onto the fenix 8 over MTP. The watch has no
# USB mass-storage mode (/Volumes/GARMIN never mounts), so this uses libmtp
# (brew install libmtp). Like the flash targets it CONSUMES an existing build:
# rebuild with the monkeyc invocation in watch/README.md.
#   make sideload                          # data field (main product path)
#   make sideload CIQ_PRJ=garmin_ctrl_app  # once that project is built
# The Apps folder id is resolved live from mtp-filetree — libmtp's name paths
# fail on Garmin ("Parent folder could not be found") and mtp-sendfile has no
# -f flag, so a bare numeric id is the only reliable form. Re-runs can leave
# duplicate app.prg files: Garmin's MTP delete is unreliable (PTP error 2002)
# and its object enumeration is stale (mtp-files can report ids mtp-filetree
# no longer shows), so the previous-file lookup below is best-effort and only
# warns. Manual cleanup: mtp-delfile -n <older-id> (retry if it 2002s).
CIQ_PRJ ?= garmin_data_field

sideload:
	@test -f watch/$(CIQ_PRJ)/out/app.prg || { echo "error: watch/$(CIQ_PRJ)/out/app.prg missing — build it first (watch/README.md)"; exit 1; }
	@mtp-detect >/dev/null 2>&1 || { echo "error: watch not found over USB (MTP mode?)"; exit 1; }
	@apps=$$(mtp-filetree 2>/dev/null | awk '$$1 ~ /^[0-9]+$$/ && $$2=="Apps" {print $$1; exit}'); \
	  test -n "$$apps" || { echo "error: no GARMIN/Apps folder on the watch"; exit 1; }; \
	  old=$$(mtp-files 2>/dev/null | awk -v apps="$$apps" '/File ID: /{id=$$3; want=0} /Filename: app\.prg/{want=1} want && /Parent ID: / && $$3==apps {print id; exit}'); \
	  test -z "$$old" || echo "note: GARMIN/Apps already reports an app.prg (id $$old) — this run adds another copy; the watch installs the newest (MTP deletes are unreliable here, so no auto-remove)"; \
	  echo "sideload watch/$(CIQ_PRJ)/out/app.prg -> GARMIN/Apps (folder id $$apps)"; \
	  mtp-sendfile watch/$(CIQ_PRJ)/out/app.prg "$$apps" 2>/dev/null | grep -q 'New file ID' || { echo "error: mtp-sendfile failed"; exit 1; }
	@newer=$$(find watch/$(CIQ_PRJ)/source watch/$(CIQ_PRJ)/resources -type f -newer watch/$(CIQ_PRJ)/out/app.prg 2>/dev/null | head -1); \
	  test -z "$$newer" || echo "note: $$newer is newer than the .prg — rebuild before testing (watch/README.md)"
	@echo "ok — unplug the watch; it installs GARMIN/Apps on eject/boot"

firmware:
	$(MAKE) -C firmware $(FW_MAKE_ARGS)

# macOS never issues SET_CONFIGURATION to the running app, so the CDC console
# node never appears and the USB-DFU route is unreachable. This does the bus
# reset + SET_CONFIGURATION(1) by hand. Needed after every app boot; the
# bootloader does not need it. Full measurement trail: docs/flashing.md §8a.
usb-kick:
	@tools/usb_kick.py

dfu settings flash-dfu flash-full flash-sd flash-app dfu-enter reset:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) $@

clean:
	$(MAKE) -C test/host clean
	$(MAKE) -C test/mock clean
	$(MAKE) -C firmware $(FW_MAKE_ARGS) clean
