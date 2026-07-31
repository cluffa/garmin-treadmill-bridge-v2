#!/usr/bin/env python3
"""ctypes ABI test for libworkout_probe.so.

Asserts that the real core/workout_ctrl.c, reached through the probe shim,
reports the actions this repo already pins down in
test/host/test_workout_ctrl.c. If this passes, the mock bridge's predicted
belt action is the firmware's actual behaviour, not a reimplementation.
"""
import ctypes
import pathlib
import struct
import sys

LIB = pathlib.Path(__file__).with_name("libworkout_probe.so")

ACT_NONE, ACT_SPEED, ACT_STOP = 0, 1, 2
TIMER_OFF, TIMER_STOPPED, TIMER_PAUSED, TIMER_ON = 0, 1, 2, 3
TGT_SPEED, TGT_HR, TGT_OPEN = 0, 1, 2


def load():
    lib = ctypes.CDLL(str(LIB))
    lib.probe_reset.argtypes = []
    lib.probe_reset.restype = None
    lib.probe_feed.argtypes = [ctypes.c_char_p, ctypes.c_uint16]
    lib.probe_feed.restype = ctypes.c_int
    lib.probe_tick.argtypes = []
    lib.probe_tick.restype = ctypes.c_int
    lib.probe_last_speed.argtypes = []
    lib.probe_last_speed.restype = ctypes.c_float
    return lib


def frame(timer, has_step, target, lo, hi, version=1):
    f = bytearray(15)
    f[0] = version
    f[1] = timer
    f[2] = 0x01 if has_step else 0x00
    f[3] = 0xFF
    f[4] = target
    struct.pack_into("<HH", f, 5, lo, hi)
    f[9] = 0xFF
    return bytes(f)


def main():
    lib = load()

    # A running speed step of 10.0 km/h (2778 mm/s) commands immediately.
    lib.probe_reset()
    act = lib.probe_feed(frame(TIMER_ON, True, TGT_SPEED, 2778, 2778), 15)
    assert act == ACT_SPEED, act
    assert 9.9 < lib.probe_last_speed() < 10.1, lib.probe_last_speed()

    # The identical frame again is deduplicated — nothing is issued.
    assert lib.probe_feed(frame(TIMER_ON, True, TGT_SPEED, 2778, 2778), 15) == ACT_NONE

    # A range commands the midpoint: 2222/2500 -> 2361 mm/s -> 8.5 km/h.
    assert lib.probe_feed(frame(TIMER_ON, True, TGT_SPEED, 2222, 2500), 15) == ACT_SPEED
    assert 8.4 < lib.probe_last_speed() < 8.6, lib.probe_last_speed()

    # Pausing stops the belt.
    assert lib.probe_feed(frame(TIMER_PAUSED, False, 0xFF, 0, 0), 15) == ACT_STOP

    # Free run (running, no step) leaves the belt alone.
    lib.probe_reset()
    assert lib.probe_feed(frame(TIMER_ON, False, 0xFF, 0, 0), 15) == ACT_NONE

    # A non-speed target holds the belt rather than commanding it.
    lib.probe_reset()
    assert lib.probe_feed(frame(TIMER_ON, True, TGT_OPEN, 0, 0), 15) == ACT_NONE

    # A bad version is dropped.
    lib.probe_reset()
    assert lib.probe_feed(frame(TIMER_ON, True, TGT_SPEED, 2778, 2778, version=9), 15) == ACT_NONE

    # The keepalive re-asserts an active speed after 30 ticks, not before.
    lib.probe_reset()
    assert lib.probe_feed(frame(TIMER_ON, True, TGT_SPEED, 2778, 2778), 15) == ACT_SPEED
    for _ in range(29):
        assert lib.probe_tick() == ACT_NONE
    assert lib.probe_tick() == ACT_SPEED
    assert 9.9 < lib.probe_last_speed() < 10.1

    # A stopped belt needs no keepalive.
    lib.probe_reset()
    assert lib.probe_feed(frame(TIMER_STOPPED, False, 0xFF, 0, 0), 15) == ACT_STOP
    for _ in range(40):
        assert lib.probe_tick() == ACT_NONE

    print("test_workout_probe: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
