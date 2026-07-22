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

# ---- TESTBOARD change detection ---------------------------------------------
# When TESTBOARD changes between builds, force a clean rebuild of all objects.
# The $(shell) runs during Makefile parsing (before any target recipes), so
# stale .o files are removed before the build graph is walked — no race with -j.
TESTBOARD_STAMP := $(OUTPUT_DIRECTORY)/.testboard_stamp

# Only evaluate at top-level (not during clean or help, which may not have _build)
ifneq ($(MAKECMDGOALS),clean)
  __tb_stale := $(shell \
    current=$$(cat $(TESTBOARD_STAMP) 2>/dev/null || echo "none"); \
    if [ "$$current" != "$(TESTBOARD)" ]; then \
      echo "  TESTBOARD=$(TESTBOARD) (changed), cleaning objects" >&2; \
      rm -f $(OUTPUT_DIRECTORY)/nrf52840_xxaa/*.o 2>/dev/null; \
      mkdir -p $(OUTPUT_DIRECTORY) 2>/dev/null; \
      echo "$(TESTBOARD)" > $(TESTBOARD_STAMP); \
    fi)
endif

# ---- Convenience targets ----------------------------------------------------
.PHONY: default help dfu flash-dfu flash-sd flash-app

default: $(TARGETS)

help:
	@echo "targets: nrf52840_xxaa flash flash_softdevice erase dfu flash-dfu"

# Signed USB-DFU package (S340 v7.0.1, hw 52)
DFU_KEY  ?= ../dfu/dfu_private_key.pem
DFU_ZIP  := $(OUTPUT_DIRECTORY)/app_dfu.zip
APP_HEX  := $(OUTPUT_DIRECTORY)/nrf52840_xxaa.hex

dfu: $(APP_HEX)
	@echo "  Creating DFU package..."
	nrfutil pkg generate \
	  --hw-version 52 --sd-req 0xCE \
	  --application $(APP_HEX) --application-version 1 \
	  --key-file $(DFU_KEY) \
	  $(DFU_ZIP)
	@echo "  DFU package: $(DFU_ZIP)"

flash-dfu:
	@test -n "$(SERIAL)" || { echo "ERROR: SERIAL=/dev/cu.usbmodemXXXX required"; exit 1; }
	nrfutil dfu usb-serial -pkg $(DFU_ZIP) -p $(SERIAL)

flash-sd:
	@echo "TODO: flash SoftDevice (nrfjprog -f nrf52 --program $(S340_HEX) ...)"

flash-app:
	@echo "TODO: flash app (nrfjprog -f nrf52 --program $(OUTPUT_DIRECTORY)/nrf52840_xxaa.hex ...)"
