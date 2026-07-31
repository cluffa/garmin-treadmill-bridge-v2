"""Decode the 15-byte workout telemetry frame the Garmin data field writes.

Layout is authoritative in core/workout_ctrl.h:20-35 and mirrored in
watch/garmin_data_field/source/DataFieldView.mc. This module only *reads* the
frame — it deliberately contains no belt-control policy. What the bridge would
actually do with a frame comes from core/workout_ctrl.c through
libworkout_probe.so, so there is exactly one implementation of that decision.

No BLE imports: this is unit-testable without bless installed.
"""
import struct

FRAME_LEN = 15
FRAME_VERSION = 1

# Only the enum values this repo actually pins down get names. Anything else
# prints as a bare number rather than inventing a CIQ constant.
#   timer states:  core/workout_ctrl.c:6-9
#   target SPEED:  core/workout_ctrl.c:10;  HR/OPEN: test/host/test_workout_ctrl.c:41-43
#   intensity:     core/workout_ctrl.h:23
#   0xFF sentinels: DataFieldView._newBaseFrame
TIMER = {0: "OFF", 1: "STOPPED", 2: "PAUSED", 3: "ON"}
TARGET = {0: "SPEED", 1: "HR", 2: "OPEN", 255: "unset"}
INTENSITY = {0: "active", 1: "rest", 255: "unset"}
DURATION = {255: "unset"}

# Width of the timestamp + tag prefix that log() emits — "HH:MM:SS.mmm" (12) +
# two spaces + a 5-wide tag + one space = 20 — so continuation lines sit under
# the message column rather than 4 characters off it.
INDENT = " " * 20


class Malformed(Exception):
    """Frame the firmware would drop (silently) on length or version."""


def _name(table, v):
    n = table.get(v)
    return f"{v}({n})" if n else str(v)


def decode(buf: bytes) -> dict:
    if len(buf) != FRAME_LEN:
        raise Malformed(f"len={len(buf)} (expected {FRAME_LEN})")
    if buf[0] != FRAME_VERSION:
        raise Malformed(f"ver={buf[0]} (expected {FRAME_VERSION})")
    lo, hi = struct.unpack_from("<HH", buf, 5)
    return {
        "version": buf[0],
        "timer": buf[1],
        "flags": buf[2],
        "intensity": buf[3],
        "target": buf[4],
        "lo_mmps": lo,
        "hi_mmps": hi,
        "dur_type": buf[9],
        "dur_value": struct.unpack_from("<I", buf, 10)[0],
        "rep": buf[14],
    }


def kmh(mmps: int) -> float:
    """mm/s -> km/h, matching workout_ctrl.c's `mmps * 0.0036f`."""
    return mmps * 0.0036


def format_frame(d: dict) -> str:
    """Two lines: state on the first, the step on the second."""
    head = (f"ver={d['version']} timer={_name(TIMER, d['timer'])} "
            f"flags=0x{d['flags']:02X} "
            f"intensity={_name(INTENSITY, d['intensity'])}")
    body = (f"tgt={_name(TARGET, d['target'])} "
            f"lo={d['lo_mmps']} hi={d['hi_mmps']} mm/s "
            f"({kmh(d['lo_mmps']):.1f}-{kmh(d['hi_mmps']):.1f} km/h) "
            f"dur={_name(DURATION, d['dur_type'])} {d['dur_value']} "
            f"rep={d['rep']}")
    return f"{head}\n{INDENT}{body}"


def annotate(d: dict) -> str:
    """Describe the frame's *content*. Not a policy decision — see module docstring.

    'FREE RUN' is called out because a free run legitimately produces no belt
    movement, which has been mistaken for a fault twice (watch/README.md).
    """
    if d["timer"] != 3:
        return "timer not running"
    if not (d["flags"] & 0x01):
        return "FREE RUN - no structured step"
    if d["target"] != 0:
        return "non-speed target"
    return "speed step"
