# Seeed XIAO nRF52840 — treadmill↔Garmin ANT+ bridge, v2 firmware.
#
# Prerequisites (paths overridable on the command line or environment):
#   SDK_ROOT  — nRF5 SDK v17.1.0
#   S340_API  — S340 v7.0.1 headers from thisisant.com
#   GNU_INSTALL_ROOT — arm-none-eabi-gcc toolchain prefix
#   GNU_VERSION      — gcc version string (e.g. 7.2.1)
#
# Build:  make
#         make -j8 GNU_INSTALL_ROOT=... GNU_VERSION=7.2.1 SDK_ROOT=... S340_API=...
#
# Task 1.2: USB-CDC ACM console + interactive ctrl dispatch.
# M2 Task 2.x: expansion-board test aid (TESTBOARD=1 links; =0 compiles to no-ops).

TESTBOARD ?= 1

# Temporary diagnostic: trace every BLE event (see docs/HANDOFF.md). 0 = off.
DIAG_BLE_EVT_TRACE ?= 0

# Temporary diagnostic: hexdump adverts stronger than MIN_RSSI dBm. 0 = off.
DIAG_ADV_DUMP ?= 0
DIAG_ADV_DUMP_MIN_RSSI ?= -70

PROJECT_NAME     := garmin_ant_bridge
TARGETS          := nrf52840_xxaa
OUTPUT_DIRECTORY := _build

SDK_ROOT  ?= $(HOME)/nRF5_SDK_17.1.0_ddde560
S340_API  ?= /Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include
S340_HEX  ?= /Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.hex

FW_DIR   := .
CORE_DIR := ../core

$(OUTPUT_DIRECTORY)/nrf52840_xxaa.out: LINKER_SCRIPT := xiao_nrf52840_s340.ld

# ---- Project sources --------------------------------------------------------
SRC_FILES += \
  $(FW_DIR)/main.c \
  $(FW_DIR)/usb_cdc_log.c \
  $(FW_DIR)/app_state.c \
  $(FW_DIR)/ant_sdm.c \
  $(FW_DIR)/ble_ctrl_svc.c \
  $(FW_DIR)/ble_central.c \
  $(FW_DIR)/last_device.c \
  $(CORE_DIR)/ctrl_dispatch.c \
  $(CORE_DIR)/ctrl_frames.c \
  $(CORE_DIR)/workout_ctrl.c \
  $(CORE_DIR)/ftms_parse.c \
  $(CORE_DIR)/ifit_parse.c \
  $(CORE_DIR)/ifit_fsm.c \
  $(CORE_DIR)/ftms_devlist.c \
  $(CORE_DIR)/connect_policy.c \
  $(CORE_DIR)/ant_sdm_encode.c \

# M2 testboard sources (conditionally linked)
ifeq ($(TESTBOARD),1)
SRC_FILES += \
  $(FW_DIR)/testboard/hw_led.c \
  $(FW_DIR)/testboard/hw_buzzer.c \
  $(FW_DIR)/testboard/hw_button.c \
  $(FW_DIR)/testboard/ssd1306.c \
  $(FW_DIR)/testboard/testboard.c
endif

# ---- SDK: startup + system --------------------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/mdk/gcc_startup_nrf52840.S \
  $(SDK_ROOT)/modules/nrfx/mdk/system_nrf52840.c \

# ---- SDK: SoftDevice handler ------------------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh_soc.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh_ant.c \
  $(SDK_ROOT)/components/softdevice/common/nrf_sdh_ble.c \

# ---- SDK: BLE common libraries ----------------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/components/ble/common/ble_advdata.c \
  $(SDK_ROOT)/components/ble/common/ble_srv_common.c \

# ---- SDK: BLE scanning module (nrf_ble_scan) --------------------------------
SRC_FILES += \
  $(SDK_ROOT)/components/ble/nrf_ble_scan/nrf_ble_scan.c \

# ---- SDK: logging (RTT backend) ---------------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_backend_rtt.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_backend_serial.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_default_backends.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_frontend.c \
  $(SDK_ROOT)/components/libraries/log/src/nrf_log_str_formatter.c \
  $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT.c \
  $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT_Syscalls_GCC.c \
  $(SDK_ROOT)/external/segger_rtt/SEGGER_RTT_printf.c \
  $(SDK_ROOT)/external/fprintf/nrf_fprintf.c \
  $(SDK_ROOT)/external/fprintf/nrf_fprintf_format.c \

# ---- SDK: core libraries ----------------------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/components/libraries/util/app_error.c \
  $(SDK_ROOT)/components/libraries/util/app_error_handler_gcc.c \
  $(SDK_ROOT)/components/libraries/util/app_error_weak.c \
  $(SDK_ROOT)/components/libraries/util/app_util_platform.c \
  $(SDK_ROOT)/components/libraries/util/nrf_assert.c \
  $(SDK_ROOT)/components/libraries/timer/app_timer2.c \
  $(SDK_ROOT)/components/libraries/timer/drv_rtc.c \
  $(SDK_ROOT)/components/libraries/atomic/nrf_atomic.c \
  $(SDK_ROOT)/components/libraries/atomic_fifo/nrf_atfifo.c \
  $(SDK_ROOT)/components/libraries/atomic_flags/nrf_atflags.c \
  $(SDK_ROOT)/components/libraries/balloc/nrf_balloc.c \
  $(SDK_ROOT)/components/libraries/memobj/nrf_memobj.c \
  $(SDK_ROOT)/components/libraries/ringbuf/nrf_ringbuf.c \
  $(SDK_ROOT)/components/libraries/strerror/nrf_strerror.c \
  $(SDK_ROOT)/components/libraries/queue/nrf_queue.c \
  $(SDK_ROOT)/components/libraries/sortlist/nrf_sortlist.c \
  $(SDK_ROOT)/components/libraries/experimental_section_vars/nrf_section_iter.c \
  $(SDK_ROOT)/components/libraries/pwr_mgmt/nrf_pwr_mgmt.c \
  $(SDK_ROOT)/components/libraries/fds/fds.c \
  $(SDK_ROOT)/components/libraries/fstorage/nrf_fstorage.c \
  $(SDK_ROOT)/components/libraries/fstorage/nrf_fstorage_sd.c \
  $(SDK_ROOT)/modules/nrfx/soc/nrfx_atomic.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_clock.c \
  $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_clock.c \

# ---- SDK: USB device (USBD) + CDC ACM ----------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_usbd.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_power.c \
  $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_power.c \
  $(SDK_ROOT)/components/libraries/usbd/app_usbd.c \
  $(SDK_ROOT)/components/libraries/usbd/app_usbd_core.c \
  $(SDK_ROOT)/components/libraries/usbd/app_usbd_string_desc.c \
  $(SDK_ROOT)/components/libraries/usbd/app_usbd_serial_num.c \
  $(SDK_ROOT)/components/libraries/usbd/class/cdc/acm/app_usbd_cdc_acm.c \

# ---- SDK: TWI (I2C) driver (for SSD1306 OLED) -------------------------------
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_twi.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_twim.c \
  $(SDK_ROOT)/integration/nrfx/legacy/nrf_drv_twi.c \

# ---- SDK: GPIOTE + app_button (for expansion board button) -------------------
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_gpiote.c \
  $(SDK_ROOT)/modules/nrfx/drivers/src/prs/nrfx_prs.c \
  $(SDK_ROOT)/components/libraries/button/app_button.c \

# ---- SDK: PWM driver (for passive buzzer) ------------------------------------
SRC_FILES += \
  $(SDK_ROOT)/modules/nrfx/drivers/src/nrfx_pwm.c \
  $(SDK_ROOT)/components/libraries/pwm/app_pwm.c \

# ---- Include paths ----------------------------------------------------------
INC_FOLDERS += \
  $(FW_DIR) \
  $(FW_DIR)/testboard \
  $(CORE_DIR) \
  $(S340_API) \
  $(S340_API)/nrf52 \
  $(SDK_ROOT)/components \
  $(SDK_ROOT)/components/ble/common \
  $(SDK_ROOT)/components/ble/nrf_ble_scan \
  $(SDK_ROOT)/components/boards \
  $(SDK_ROOT)/components/libraries/atomic \
  $(SDK_ROOT)/components/libraries/atomic_fifo \
  $(SDK_ROOT)/components/libraries/atomic_flags \
  $(SDK_ROOT)/components/libraries/balloc \
  $(SDK_ROOT)/components/libraries/button \
  $(SDK_ROOT)/components/libraries/delay \
  $(SDK_ROOT)/components/libraries/experimental_section_vars \
  $(SDK_ROOT)/components/libraries/fds \
  $(SDK_ROOT)/components/libraries/fstorage \
  $(SDK_ROOT)/components/libraries/log \
  $(SDK_ROOT)/components/libraries/log/src \
  $(SDK_ROOT)/components/libraries/memobj \
  $(SDK_ROOT)/components/libraries/mutex \
  $(SDK_ROOT)/components/libraries/pwm \
  $(SDK_ROOT)/components/libraries/pwr_mgmt \
  $(SDK_ROOT)/components/libraries/queue \
  $(SDK_ROOT)/components/libraries/ringbuf \
  $(SDK_ROOT)/components/libraries/sortlist \
  $(SDK_ROOT)/components/libraries/strerror \
  $(SDK_ROOT)/components/libraries/timer \
  $(SDK_ROOT)/components/libraries/usbd \
  $(SDK_ROOT)/components/libraries/usbd/class/cdc \
  $(SDK_ROOT)/components/libraries/usbd/class/cdc/acm \
  $(SDK_ROOT)/components/libraries/util \
  $(SDK_ROOT)/components/softdevice/common \
  $(SDK_ROOT)/components/toolchain/cmsis/include \
  $(SDK_ROOT)/external/fprintf \
  $(SDK_ROOT)/external/segger_rtt \
  $(SDK_ROOT)/external/utf_converter \
  $(SDK_ROOT)/integration/nrfx \
  $(SDK_ROOT)/integration/nrfx/legacy \
  $(SDK_ROOT)/modules/nrfx \
  $(SDK_ROOT)/modules/nrfx/drivers/include \
  $(SDK_ROOT)/modules/nrfx/hal \
  $(SDK_ROOT)/modules/nrfx/mdk \

LIB_FILES +=

# ---- C flags ----------------------------------------------------------------
CFLAGS += -O3 -g3
CFLAGS += -mcpu=cortex-m4 -mthumb -mabi=aapcs
CFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS += -Wall -Werror
CFLAGS += -ffunction-sections -fdata-sections -fno-strict-aliasing
CFLAGS += -fno-builtin -fshort-enums
CFLAGS += -DBOARD_CUSTOM
CFLAGS += -DCONFIG_GPIO_AS_PINRESET
CFLAGS += -DFLOAT_ABI_HARD
CFLAGS += -DNRF52840_XXAA
CFLAGS += -DS340
CFLAGS += -DSOFTDEVICE_PRESENT
CFLAGS += -DNRF_SD_BLE_API_VERSION=7
CFLAGS += -DUSE_APP_CONFIG
CFLAGS += -DAPP_TIMER_V2 -DAPP_TIMER_V2_RTC1_ENABLED
CFLAGS += -DTESTBOARD=$(TESTBOARD)
CFLAGS += -DDIAG_BLE_EVT_TRACE=$(DIAG_BLE_EVT_TRACE)
CFLAGS += -DDIAG_ADV_DUMP=$(DIAG_ADV_DUMP)
CFLAGS += -DDIAG_ADV_DUMP_MIN_RSSI=$(DIAG_ADV_DUMP_MIN_RSSI)
# ANT SoftDevice evaluation license key (nrf_sdm.h) — personal use only; a
# commercial release requires a license from thisisant.com. The key is tied to
# a developer account, so it lives in the git-ignored ant_license.mk (copy
# ant_license.mk.example and paste your own). Without it, ANT_LICENSE_KEY is
# empty and sd_ant_enable() will fail at runtime.
-include ant_license.mk
ifneq ($(ANT_LICENSE_KEY),)
CFLAGS += -DANT_LICENSE_KEY='"$(ANT_LICENSE_KEY)"'
else
$(warning ant_license.mk not found or ANT_LICENSE_KEY empty — ANT will not enable at runtime. Copy ant_license.mk.example.)
endif

# ---- Assembler flags --------------------------------------------------------
ASMFLAGS += -g3 -mcpu=cortex-m4 -mthumb -mabi=aapcs
ASMFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
ASMFLAGS += -DBOARD_CUSTOM -DCONFIG_GPIO_AS_PINRESET -DFLOAT_ABI_HARD
ASMFLAGS += -DNRF52840_XXAA -DS340 -DSOFTDEVICE_PRESENT
ASMFLAGS += -DNRF_SD_BLE_API_VERSION=7 -DUSE_APP_CONFIG
ASMFLAGS += -DAPP_TIMER_V2 -DAPP_TIMER_V2_RTC1_ENABLED
ASMFLAGS += -DTESTBOARD=$(TESTBOARD)

# ---- Linker flags -----------------------------------------------------------
LDFLAGS += -O3 -g3
LDFLAGS += -mthumb -mabi=aapcs -L$(SDK_ROOT)/modules/nrfx/mdk -T$(LINKER_SCRIPT)
LDFLAGS += -mcpu=cortex-m4 -mfloat-abi=hard -mfpu=fpv4-sp-d16
LDFLAGS += -Wl,--gc-sections
LDFLAGS += --specs=nano.specs

nrf52840_xxaa: CFLAGS += -D__HEAP_SIZE=8192 -D__STACK_SIZE=8192
nrf52840_xxaa: ASMFLAGS += -D__HEAP_SIZE=8192 -D__STACK_SIZE=8192

LIB_FILES += -lc -lnosys -lm

# ---- SDK build machinery ----------------------------------------------------
TEMPLATE_PATH := $(SDK_ROOT)/components/toolchain/gcc
include $(TEMPLATE_PATH)/Makefile.common
$(foreach target, $(TARGETS), $(call define_target, $(target)))

# ---- ANT license key dependency ---------------------------------------------
# nrf_sdh.c consumes ANT_LICENSE_KEY from ant_license.mk via -D; force a
# recompile when the key file appears or changes.
_ANT_LICENSE_MK := $(wildcard ant_license.mk)
ifneq ($(_ANT_LICENSE_MK),)
$(OUTPUT_DIRECTORY)/nrf52840_xxaa/nrf_sdh.c.o: $(_ANT_LICENSE_MK)
endif

# ---- TESTBOARD change detection ---------------------------------------------
# When TESTBOARD changes between builds, force a clean rebuild of all objects.
# The $(shell) runs during Makefile parsing (before any target recipes), so
# stale .o files are removed before the build graph is walked — no race with -j.
BUILD_STAMP := $(OUTPUT_DIRECTORY)/.build_stamp

# Goals that CONSUME an existing build instead of producing one. The $(shell)
# below runs at parse time for whatever goal was named, so without this the
# documented `make dfu` (which carries none of the canonical toolchain vars)
# sees a changed config and deletes all 80 objects out from under the very hex
# it is about to package — correct output, but a full recompile every time.
NONBUILD_GOALS := clean help dfu flash-dfu settings flash-full flash-sd flash-app dfu-enter reset

# Stamp only when at least one goal actually builds (bare `make` = default = build).
ifeq ($(strip $(MAKECMDGOALS)),)
  __do_stamp := 1
else
  __do_stamp := $(strip $(filter-out $(NONBUILD_GOALS),$(MAKECMDGOALS)))
endif

ifneq ($(__do_stamp),)
  __stale := $(shell \
    current=$$(cat $(BUILD_STAMP) 2>/dev/null || echo "none"); \
    expected="TESTBOARD=$(TESTBOARD)|CC=$(GNU_INSTALL_ROOT)|VER=$(GNU_VERSION)|EVT=$(DIAG_BLE_EVT_TRACE)|ADV=$(DIAG_ADV_DUMP)|RSSI=$(DIAG_ADV_DUMP_MIN_RSSI)"; \
    if [ "$$current" != "$$expected" ]; then \
      echo "  Build config changed ($$expected) — cleaning objects" >&2; \
      rm -f $(OUTPUT_DIRECTORY)/nrf52840_xxaa/*.o 2>/dev/null; \
      mkdir -p $(OUTPUT_DIRECTORY) 2>/dev/null; \
      echo "$$expected" > $(BUILD_STAMP); \
    fi)
endif

# ---- Flash / DFU convenience targets ----------------------------------------
# The XIAO nRF52840 has NO onboard debugger, so J-Link / nrfjprog are unusable.
# SWD flashing goes through a Raspberry Pi Pico running CMSIS-DAP (debugprobe)
# driven by pyocd; USB updates go through the Nordic Secure DFU bootloader via
# nrfutil. Full procedure + wiring: docs/flashing.md.
.PHONY: default help dfu flash-dfu flash-sd flash-app flash-full settings dfu-enter reset

default: $(TARGETS)

help:
	@echo "build:  nrf52840_xxaa"
	@echo "swd:    flash-full (S340+app+bl+settings, chip erase) | flash-app (app only)"
	@echo "        flash-sd (SoftDevice only) | settings | dfu-enter | reset"
	@echo "usb:    dfu (build signed pkg) | flash-dfu SERIAL=/dev/cu.usbmodemXXXX"
	@echo "see:    docs/flashing.md"

# ---- Tool / artifact locations (overridable) --------------------------------
PYOCD          ?= pyocd
PYOCD_TARGET   ?= nrf52840
NRFUTIL        ?= nrfutil
BOOTLOADER_HEX ?= /Users/alex/workspace/nrf52/build/bootloader_usb_s340.hex

DFU_KEY      ?= ../dfu/dfu_private_key.pem
DFU_ZIP      := $(OUTPUT_DIRECTORY)/app_dfu.zip
APP_HEX      := $(OUTPUT_DIRECTORY)/nrf52840_xxaa.hex
SETTINGS_HEX := $(OUTPUT_DIRECTORY)/settings.hex

# Bootloader settings page. MUST be the full 8-digit address: `pyocd erase
# --sector 0xFF000` treats a short address as unaligned and silently erases page
# 0x00000000 instead (see docs/flashing.md).
SETTINGS_ADDR := 0x000FF000

# GPREGRET DFU-start magic (BOOTLOADER_DFU_START) + POWER->GPREGRET address.
GPREGRET_ADDR  := 0x4000051C
GPREGRET_DFU   := 0xB1

# ---- Signed USB-DFU package (S340 v7.0.1, hw 52) ----------------------------
dfu:
	@test -f $(APP_HEX) || { echo "ERROR: $(APP_HEX) not found — build first (see CLAUDE.md)"; exit 1; }
	@test -f $(DFU_KEY) || { echo "ERROR: signing key $(DFU_KEY) not found. Copy dfu/dfu_private_key.pem.example or set DFU_KEY="; exit 1; }
	@echo "  Creating DFU package..."
	@rm -f $(DFU_ZIP)
	$(NRFUTIL) pkg generate \
	  --hw-version 52 --sd-req 0xCE \
	  --application $(APP_HEX) --application-version 1 \
	  --key-file $(DFU_KEY) \
	  $(DFU_ZIP)
	@test -f $(DFU_ZIP) || { echo "ERROR: nrfutil pkg generate failed — no package produced"; exit 1; }
	@echo "  DFU package: $(DFU_ZIP)"

# Push a signed package over USB to the Nordic bootloader (no SWD needed).
# Enter DFU first with `make dfu-enter` (or an already-invalid app auto-enters).
flash-dfu:
	@test -n "$(SERIAL)" || { echo "ERROR: SERIAL=/dev/cu.usbmodemXXXX required (see docs/flashing.md §6)"; exit 1; }
	@test -f $(DFU_ZIP) || { echo "ERROR: $(DFU_ZIP) not found — run 'make dfu' first"; exit 1; }
	$(NRFUTIL) dfu usb-serial -pkg $(DFU_ZIP) -p $(SERIAL)

# ---- SWD flashing via pyocd (Pico CMSIS-DAP probe) --------------------------
# Bootloader settings page: marks the app valid so the bootloader boots it.
# --no-backup is REQUIRED — the default backup page (0xFE000) collides with the
# MBR params page.
settings:
	@test -f $(APP_HEX) || { echo "ERROR: $(APP_HEX) not found — build first (see CLAUDE.md)"; exit 1; }
	@rm -f $(SETTINGS_HEX)
	$(NRFUTIL) settings generate --family NRF52840 \
	  --application $(APP_HEX) \
	  --application-version 1 --bootloader-version 1 \
	  --bl-settings-version 2 --no-backup \
	  $(SETTINGS_HEX)
	@test -f $(SETTINGS_HEX) || { echo "ERROR: nrfutil settings generate failed — no settings hex produced"; exit 1; }

# First-time provisioning: SoftDevice + app + bootloader + settings in one
# chip-erase pass (ERASEALL also clears UICR; the bootloader hex rewrites the
# UICR NRFFW pointers). Makes the board bootable AND updatable over USB.
flash-full: settings
	$(PYOCD) flash -t $(PYOCD_TARGET) --erase chip \
	  $(S340_HEX) $(APP_HEX) $(BOOTLOADER_HEX) $(SETTINGS_HEX)
	$(PYOCD) reset -t $(PYOCD_TARGET)

# SoftDevice only (chip-erases; follow with flash-app or use flash-full instead).
flash-sd:
	$(PYOCD) flash -t $(PYOCD_TARGET) --erase chip $(S340_HEX)

# ⚠⚠ BROKEN ON THIS SETUP — PREFER flash-full. ⚠⚠
#
# flash-app reproducibly leaves the board parked in the bootloader at
# pc=0x000f8308, app never started (.bss not zeroed, s_heartbeat_cnt garbage).
# Reproduced three times on 2026-07-29, twice deliberately, with both a growing
# and a shrinking image. flash-full has never failed. Root cause NOT found.
#
# What is measured, so nobody re-derives it:
#
#  * The settings page holds the app's validation CRC (App Boot Validation Type
#    is 1 = generated CRC), so an app flashed against a settings page describing
#    a different app fails the bootloader's boot validation.
#  * Passing $(APP_HEX) $(SETTINGS_HEX) to ONE `pyocd flash` skipped the settings
#    page: pyocd reported "identical 4096 bytes (1 page)" and programmed only the
#    app's 25 pages, leaving flash at app CRC 1afcbb43 while the freshly
#    generated settings.hex held 505dcd7e. Hence the explicit sector erase below,
#    which does change skip -> program ("identical 0 pages").
#  * BUT that is NOT sufficient. After erasing and programming the settings page
#    for a TESTBOARD=0 image (size 0x00015EC4, CRC d4d9eb5d), flash 0x000FF000
#    read back size 0x00018FE4 / CRC 505dcd7e — the PREVIOUS image's values —
#    even though pyocd reported programming that page. Something restores or
#    overwrites the settings page after pyocd writes it and before/while the
#    bootloader runs. The bootloader itself is the obvious suspect (0x000FE000 is
#    the MBR params page; --no-backup is already passed to nrfutil), but this is
#    unproven.
#
# Next step for whoever picks this up: read 0x000FF000 immediately after the
# `pyocd flash` of the settings page but BEFORE `pyocd reset`. That single
# measurement separates "pyocd never wrote it" from "the bootloader overwrote
# it", and nobody has taken it yet.
flash-app: settings
	@echo "⚠ flash-app is BROKEN on this setup — it parks in the bootloader."
	@echo "⚠ Use 'make flash-full' instead. Continuing in 3s; Ctrl-C to abort."
	@sleep 3
	$(PYOCD) flash -t $(PYOCD_TARGET) --erase sector $(APP_HEX)
	$(PYOCD) erase -t $(PYOCD_TARGET) --sector $(SETTINGS_ADDR)
	$(PYOCD) flash -t $(PYOCD_TARGET) --erase sector $(SETTINGS_HEX)
	$(PYOCD) reset -t $(PYOCD_TARGET)
	@echo ""
	@echo "flash-app: NOW VERIFY THE APP ACTUALLY STARTED —"
	@echo "  HB=\$$(nm -S $(OUTPUT_DIRECTORY)/nrf52840_xxaa.out | grep ' s_heartbeat_cnt' | awk '{print \"0x\"\$$1}')"
	@echo "  $(PYOCD) commander -t $(PYOCD_TARGET) -O connect_mode=attach -c \"read32 \$$HB\""
	@echo "  Small monotonically increasing value = the app booted."
	@echo "  Garbage = .bss never zeroed, bootloader kept control -> run flash-full."

# Kick the running app into the DFU bootloader over SWD: set GPREGRET magic and
# do a *software* reset (SYSRESETREQ preserves GPREGRET; POR/pin reset clears it).
dfu-enter:
	$(PYOCD) commander -t $(PYOCD_TARGET) \
	  -c "halt" -c "write32 $(GPREGRET_ADDR) $(GPREGRET_DFU)" -c "reset -t sw"

reset:
	$(PYOCD) reset -t $(PYOCD_TARGET)
