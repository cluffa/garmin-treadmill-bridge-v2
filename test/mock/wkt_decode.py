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

# WORKOUT_INTENSITY_REST — the one intensity the bridge acts on, by dropping
# to a walk when the step carries no speed target (core/workout_ctrl.c).
REST_INTENSITY = 1

# Width of the timestamp + tag prefix that log() emits — "HH:MM:SS.mmm" (12) +
# two spaces + a 5-wide tag + one space = 20 — so continuation lines sit under
# the message column rather than 4 characters off it.
INDENT = " " * 20


class Malformed(Exception):
    """Frame the firmware would drop (silently): shorter than FRAME_LEN, or the
    wrong version. A frame *longer* than FRAME_LEN is not malformed — see
    decode()'s docstring.
    """


def _name(table, v):
    n = table.get(v)
    return f"{v}({n})" if n else str(v)


def decode(buf: bytes) -> dict:
    """Decode the leading FRAME_LEN bytes of `buf`.

    Matches core/workout_ctrl.c:76's `len < WORKOUT_FRAME_LEN` check: only a
    frame *shorter* than FRAME_LEN is rejected. A longer frame is accepted and
    decoded from its first FRAME_LEN bytes, exactly as workout_ctrl_on_frame()
    would act on it — the trailing bytes are reported via "extra_bytes" for the
    caller to log, not silently dropped, since decoding fewer frames than the
    firmware would act on is itself a divergence worth catching.
    """
    if len(buf) < FRAME_LEN:
        raise Malformed(f"len={len(buf)} (expected >= {FRAME_LEN})")
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
        "extra_bytes": len(buf) - FRAME_LEN,
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

    The no-step case is called out because it legitimately produces no belt
    movement, which has been mistaken for a fault twice (watch/README.md). It is
    split in three: all-sentinel step fields really are "no step" (a free run, or
    a step the field could not resolve — the frame cannot distinguish those); a
    populated intensity of REST means a rest step, which the bridge answers by
    dropping to its walk speed; any other populated intensity/target means a
    structured step WAS visible and the field simply did not flag it as a speed
    target, which the bridge answers by holding.

    An oversized frame (extra_bytes > 0) gets a trailing note — worth knowing
    about, but not a rejection; the firmware acts on its first FRAME_LEN bytes
    same as decode() did here.

    A speed-target frame with lo=hi=0 gets its own label rather than being
    folded into the generic "speed step" case: `decode_action()` resolves
    lo=hi=0 to a 0 mm/s midpoint and treats it as no target at all, so an
    active step with lo=hi=0 falls through to ACT_NONE — the same "don't touch
    the belt" outcome as a free run, just reached from a structured step
    instead of no step at all. (On a REST step it instead falls through to the
    walk speed.) Calling this "speed step" would make mock_bridge.py print the
    self-contradicting `-> no change (deduplicated, or belt held)
    [speed step]`, in exactly the "why isn't the belt moving" situation that
    has already cost this project debugging time twice. This function still
    only *describes* the frame — the
    actual ACT_NONE/ACT_SPEED decision is decoded elsewhere, from the real
    core/workout_ctrl.c via libworkout_probe.so, per the module docstring.
    """
    extra = d.get("extra_bytes", 0)
    note = f" [+{extra}B oversized, ignored]" if extra else ""
    if d["timer"] != 3:
        return "timer not running" + note
    if not (d["flags"] & 0x01):
        # flags bit0 clear only means "the field did not report a speed step".
        # That is a free run ONLY when the step fields are all sentinels too.
        # If intensity or targetType carry real values, a structured step was
        # plainly visible to the watch and DataFieldView._packFrame returned
        # before setting the flag — a different situation with a different fix,
        # and labelling it "FREE RUN" sends the reader down the wrong path.
        # Observed on hardware 2026-07-31: a workout's rest steps arrive as
        # intensity=1(rest) tgt=2(OPEN) flags=0x00. See
        # docs/superpowers/test-logs/2026-07-31-mock-bridge-bringup.md.
        #
        # Diagnostic bits 1-3 (FLAG_SRC_* in DataFieldView.mc) say *why* a
        # no-step frame was emitted. The bridge ignores them, so their presence
        # identifies the exact _packFrame return path on the watch.
        if d["flags"] & 0x02:
            # bit4 (0x10) narrows the throw to the duration-value region.
            where = " (duration-value region)" if d["flags"] & 0x10 else ""
            return "no speed step (pack threw an exception" + where + ")" + note
        if d["flags"] & 0x04:
            return "no speed step (no workout step resolved)" + note
        if d["flags"] & 0x08:
            return "no speed step (step resolved but targetType missing)" + note
        if d["intensity"] == 0xFF and d["target"] == 0xFF:
            return "no step reported - free run, or the field could not resolve the step" + note
        if d["intensity"] == REST_INTENSITY:
            # Called out separately from the generic no-speed-target case
            # because the two now diverge in the belt: a rest step drops to the
            # walk speed, anything else holds. Still a description of the
            # frame, not the decision — see the module docstring.
            return "rest step, no speed target" + note
        return "structured step present but not flagged as a speed target" + note
    if d["target"] != 0:
        return "non-speed target" + note
    if d["lo_mmps"] == 0 and d["hi_mmps"] == 0:
        return "speed step, lo=hi=0 (no resolvable speed)" + note
    return "speed step" + note
