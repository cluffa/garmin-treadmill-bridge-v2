# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
#
# Every path below is overridable: `make SDK_ROOT=/elsewhere firmware` works,
# because these are ?= defaults that a command-line assignment beats, and the
# resolved values are what get forwarded to the sub-make. (Passing a literal
# on the sub-make command line, as this file used to, silently outranks
# anything the user sets.)

.PHONY: host-test firmware dfu settings flash-dfu flash-full flash-sd flash-app \
        dfu-enter reset clean help mock-bridge mock-test sideload usb-kick \
        pace-test pace-report pace-baseline ciq-build

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
	@echo "  make pace-test              score belt pace tracking/lag from a .FIT vs baseline"
	@echo "  make pace-report FIT=x.fit  full pace/lag report for one recorded workout"
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

# Pace-tracking regression gate. Scores a .FIT recorded in SDM:TGT mode against
# what the bridge should have commanded, and compares it to a stored baseline.
# See docs/pace-lag-analysis.md for the method and the current numbers.
#   make pace-test                                    # scorer self-check + baseline
#   make pace-test FIT=test/my-new-run.fit            # score a new run
#   make pace-report FIT=test/my-new-run.fit          # full report, no gate
#   make pace-baseline FIT=test/my-new-run.fit BASELINE=test/baselines/x.json
# Default is the 2026-08-03 POST-fix trace: 4 km/h rest steps, spread ANT
# background pages, page-2 use state active. effective_lag 1.371 s.
FIT      ?= test/23842067586_ACTIVITY.fit
BASELINE ?= test/baselines/23842067586-post-fix.json
# The hole-periodicity check must use the page-cycle length of the firmware that
# RECORDED the trace, not the one in the tree today. Empty means "read
# CYCLE_LEN x SDM_CHANNEL_PERIOD from firmware/ant_sdm.c", which is correct for
# the post-fix default and stays correct as that file changes.
#
# Re-scoring the pre-fix trace needs its recording firmware's values passed
# explicitly — it predates the background-page spread (68 slots x 0.25 s):
#   make pace-test FIT=test/23806153959_ACTIVITY.fit \
#                  BASELINE=test/baselines/23806153959-pre-fix.json SDM_CYCLE=17.0
SDM_CYCLE ?=
PACE_ARGS = $(if $(SDM_CYCLE),--sdm-cycle-s $(SDM_CYCLE))

pace-test:
	./test/pace_lag_report.py --self-test
	./test/pace_lag_report.py $(FIT) -q $(PACE_ARGS) --baseline $(BASELINE)

pace-report:
	./test/pace_lag_report.py $(FIT) -v $(PACE_ARGS)

pace-baseline:
	./test/pace_lag_report.py $(FIT) -q $(PACE_ARGS) --write-baseline $(BASELINE)

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
# The mechanics live in tools/ciq_sideload.sh — every MTP call needs a timeout
# (Garmin's stack blocks forever instead of erroring) and that is too much shell
# to keep legible in a recipe. Notable knobs, all documented in the script:
#   SIDELOAD_FORCE=1        send even when sources are newer than the .prg
#   SIDELOAD_CHECK_DUPES=1  opt into the slow duplicate-app.prg scan
CIQ_PRJ ?= garmin_data_field

sideload:
	@tools/ciq_sideload.sh watch/$(CIQ_PRJ)

# Build a Connect IQ project. Regenerates BuildInfo.mc (the stamp the data
# field renders on its bottom row) and THEN runs monkeyc, so the stamp on the
# watch always names the build it is part of — the whole point is that a
# sideload which silently did not take stops being invisible.
#   make ciq-build                          # data field
#   make ciq-build CIQ_PRJ=garmin_ctrl_app
# monkeyc is not on PATH; override CIQ_SDK/CIQ_KEY/CIQ_DEV as needed.
CIQ_SDK ?= $(HOME)/Library/Application Support/Garmin/ConnectIQ/Sdks/connectiq-sdk-mac-9.2.0-2026-06-09-92a1605b2
CIQ_KEY ?= $(HOME)/Documents/garmin_developer_key.der
CIQ_DEV ?= fenix8solar51mm

ciq-build:
	@test -x "$(CIQ_SDK)/bin/monkeyc" || { echo "error: no monkeyc at $(CIQ_SDK)/bin — set CIQ_SDK (see watch/README.md)"; exit 1; }
	@test -f "$(CIQ_KEY)" || { echo "error: developer key $(CIQ_KEY) not found — set CIQ_KEY"; exit 1; }
	@tools/ciq_stamp.sh watch/$(CIQ_PRJ)
	@cd watch/$(CIQ_PRJ) && "$(CIQ_SDK)/bin/monkeyc" -f monkey.jungle \
	    -o out/app.prg -y "$(CIQ_KEY)" -d $(CIQ_DEV) -l 2
	@echo "ok — built watch/$(CIQ_PRJ)/out/app.prg for $(CIQ_DEV)"

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
