# Builds the shared library that lets mock_bridge.py run frames through the
# firmware's own core/workout_ctrl.c instead of a Python reimplementation.
CORE   = ../../core
CFLAGS = -I$(CORE) -Wall -Wextra -std=c11 -fPIC
LIB    = libworkout_probe.so

.PHONY: all test clean

all: $(LIB)

$(LIB): workout_probe.c $(CORE)/workout_ctrl.c
	$(CC) $(CFLAGS) -shared -o $@ $^ -lm

test: $(LIB)
	python3 test_workout_probe.py

clean:
	rm -f $(LIB)
