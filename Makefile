# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
# Individual sub-Makefiles are filled in by later tasks.

.PHONY: host-test firmware dfu flash-dfu flash-sd flash-app clean

host-test:
	$(MAKE) -C test/host

firmware:
	@echo "TODO: make -C firmware"

dfu:
	@echo "TODO: make -C firmware dfu"

flash-dfu:
	@echo "TODO: make -C firmware flash-dfu"

flash-sd:
	@echo "TODO: make -C firmware flash-sd"

flash-app:
	@echo "TODO: make -C firmware flash-app"

clean:
	$(MAKE) -C test/host clean
	@echo "TODO: make -C firmware clean"
