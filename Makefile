# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
#
# Every path below is overridable: `make SDK_ROOT=/elsewhere firmware` works,
# because these are ?= defaults that a command-line assignment beats, and the
# resolved values are what get forwarded to the sub-make. (Passing a literal
# on the sub-make command line, as this file used to, silently outranks
# anything the user sets.)

.PHONY: host-test firmware dfu settings flash-dfu flash-full flash-sd flash-app \
        dfu-enter reset clean help

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
	@echo "  make firmware               build the nRF52840 image"
	@echo "Watch (Connect IQ) — see watch/README.md:"
	@echo "  watch/garmin_data_field     writes workout targets to A6ED0004"
	@echo "  watch/garmin_ctrl_app       SCAN/CONNECT picker UI (A6ED0002/0003)"
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

# The watch discovers the bridge by filtering on the 128-bit A6ED service UUID,
# so firmware/mock/CIQ disagreement is SILENT — the watch just never finds the
# device. Cheap to check, so it runs as part of the standard gate.
check-uuid:
	@python3 test/check_uuid_contract.py

firmware:
	$(MAKE) -C firmware $(FW_MAKE_ARGS)

dfu settings flash-dfu flash-full flash-sd flash-app dfu-enter reset:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) $@

clean:
	$(MAKE) -C test/host clean
	$(MAKE) -C firmware $(FW_MAKE_ARGS) clean
