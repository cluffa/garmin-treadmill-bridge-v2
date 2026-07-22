# Top-level dispatch Makefile for garmin-treadmill-bridge-v2
# Individual sub-Makefiles are filled in by later tasks.

.PHONY: host-test firmware dfu flash-dfu flash-sd flash-app clean

host-test:
	@echo "TODO: make -C test/host"

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
	@echo "TODO: make -C test/host clean; make -C firmware clean"
