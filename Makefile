# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
# Individual sub-Makefiles are filled in by later tasks.

.PHONY: host-test firmware dfu flash-dfu flash-sd flash-app clean

host-test:
	$(MAKE) -C test/host

FW_MAKE_ARGS = \
  GNU_INSTALL_ROOT=/Users/alex/.platformio/packages/toolchain-gccarmnoneeabi/bin/ \
  GNU_VERSION=7.2.1 \
  SDK_ROOT=/Users/alex/nRF5_SDK_17.1.0_ddde560 \
  S340_API=/Users/alex/workspace/nrf52/ANT_s340_nrf52_7.0.1/ANT_s340_nrf52_7.0.1.API/include

firmware:
	$(MAKE) -C firmware $(FW_MAKE_ARGS)

dfu:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) dfu

flash-dfu:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) flash-dfu

flash-sd:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) flash-sd

flash-app:
	$(MAKE) -C firmware $(FW_MAKE_ARGS) flash-app

clean:
	$(MAKE) -C test/host clean
	$(MAKE) -C firmware clean
