#!/usr/bin/env python3
"""Unit tests for the workout-frame decoder used by mock_bridge.py."""
import struct
import sys

import wkt_decode as w


def frame(timer, flags, target, lo, hi, version=1, intensity=0xFF,
          dur_type=0xFF, dur_value=0, rep=0):
    f = bytearray(15)
    f[0] = version
    f[1] = timer
    f[2] = flags
    f[3] = intensity
    f[4] = target
    struct.pack_into("<HH", f, 5, lo, hi)
    f[9] = dur_type
    struct.pack_into("<I", f, 10, dur_value)
    f[14] = rep
    return bytes(f)


def main():
    # Every field round-trips out of the little-endian layout.
    d = w.decode(frame(3, 0x01, 0, 2222, 2500, intensity=0,
                       dur_type=5, dur_value=300, rep=2))
    assert d["version"] == 1
    assert d["timer"] == 3
    assert d["flags"] == 0x01
    assert d["intensity"] == 0
    assert d["target"] == 0
    assert d["lo_mmps"] == 2222
    assert d["hi_mmps"] == 2500
    assert d["dur_type"] == 5
    assert d["dur_value"] == 300
    assert d["rep"] == 2

    # mm/s -> km/h.
    assert abs(w.kmh(2778) - 10.0) < 0.01
    assert w.kmh(0) == 0.0

    # A short frame and a bad version are both rejected, with the reason in the
    # message — the firmware drops these silently, so the mock must not.
    for bad, needle in [(frame(3, 0x01, 0, 2222, 2500)[:12], "len=12"),
                        (frame(3, 0x01, 0, 2222, 2500, version=9), "ver=9")]:
        try:
            w.decode(bad)
        except w.Malformed as e:
            assert needle in str(e), str(e)
        else:
            raise AssertionError(f"expected Malformed for {needle}")

    # A 14-byte frame (one short of FRAME_LEN) is still rejected — the boundary
    # is strictly "shorter than FRAME_LEN", matching core/workout_ctrl.c:76's
    # `len < WORKOUT_FRAME_LEN`.
    try:
        w.decode(frame(3, 0x01, 0, 2222, 2500)[:14])
    except w.Malformed as e:
        assert "len=14" in str(e), str(e)
    else:
        raise AssertionError("expected Malformed for a 14-byte frame")

    # A 16-byte frame (one longer than FRAME_LEN) is NOT rejected: the firmware
    # accepts len >= WORKOUT_FRAME_LEN and acts on the frame, so the mock must
    # decode it too rather than reporting MALFORMED for something the bridge
    # would actually obey. The extra byte is still surfaced, as a note rather
    # than a rejection.
    oversized = frame(3, 0x01, 0, 2222, 2500) + b"\x00"
    d = w.decode(oversized)
    assert d["extra_bytes"] == 1
    assert d["timer"] == 3 and d["target"] == 0  # decoded from the first FRAME_LEN bytes
    assert "oversized" in w.annotate(d)

    # Content annotations describe the frame, not the belt policy.
    # All-sentinel step fields with the flag clear: a genuine free run (or a
    # step the field could not resolve at all — the frame cannot tell them apart).
    assert "free run" in w.annotate(w.decode(frame(3, 0x00, 0xFF, 0, 0)))

    # Flag clear but the step fields carry real values: NOT a free run. This is
    # the shape a workout's rest step actually arrives in — observed on hardware
    # 2026-07-31 as intensity=1(rest) tgt=2(OPEN) flags=0x00. Labelling it
    # "free run" would point the reader at the wrong bug, and it is the one
    # frame shape the bridge answers by dropping to the walk speed.
    rest_step = w.annotate(w.decode(frame(3, 0x00, 2, 0, 0, intensity=1)))
    assert "free run" not in rest_step, rest_step
    assert rest_step == "rest step, no speed target", rest_step

    # Same shape but an ACTIVE step: no speed target and no rest, so the bridge
    # holds. It must not borrow the rest label.
    active_open = w.annotate(w.decode(frame(3, 0x00, 2, 0, 0, intensity=0)))
    assert active_open == "structured step present but not flagged as a speed target", \
        active_open
    assert w.annotate(w.decode(frame(2, 0x00, 0xFF, 0, 0))) == "timer not running"
    assert w.annotate(w.decode(frame(3, 0x01, 2, 0, 0))) == "non-speed target"
    assert w.annotate(w.decode(frame(3, 0x01, 0, 2222, 2500))) == "speed step"

    # Step fields all sentinel + a FLAG_SRC_* diagnostic bit: the frame says
    # *why* no step made it out (see DataFieldView.mc _packFrame).
    assert "pack threw an exception" in w.annotate(w.decode(frame(3, 0x02, 0xFF, 0, 0)))
    assert "duration-value region" in w.annotate(w.decode(frame(3, 0x12, 0xFF, 0, 0)))
    assert "no workout step resolved" in w.annotate(w.decode(frame(3, 0x04, 0xFF, 0, 0)))
    assert "targetType missing" in w.annotate(w.decode(frame(3, 0x08, 0xFF, 0, 0)))

    # A SPEED-target step with lo=hi=0 must NOT be labeled "speed step":
    # decode_action() resolves it to a 0 mm/s midpoint and treats it as no
    # target, so an active step falls through to ACT_NONE — the same "belt
    # held" outcome as a free run, just from a structured step. The old generic
    # label made the log print the self-contradicting "-> no change ...
    # [speed step]".
    assert w.annotate(w.decode(frame(3, 0x01, 0, 0, 0))) == \
        "speed step, lo=hi=0 (no resolvable speed)"
    # A one-sided target (only one of lo/hi zero) is still an ordinary speed
    # step — decode_action() resolves it to the nonzero side.
    assert w.annotate(w.decode(frame(3, 0x01, 0, 0, 2500))) == "speed step"
    assert w.annotate(w.decode(frame(3, 0x01, 0, 2222, 0))) == "speed step"

    # Formatting names the values this repo actually pins down, and leaves
    # anything else raw rather than inventing CIQ enum names.
    text = w.format_frame(w.decode(frame(3, 0x01, 0, 2222, 2500, intensity=0)))
    assert "timer=3(ON)" in text
    assert "flags=0x01" in text
    assert "intensity=0(active)" in text
    assert "tgt=0(SPEED)" in text
    assert "lo=2222 hi=2500 mm/s" in text
    assert "8.0" in text and "9.0" in text
    raw = w.format_frame(w.decode(frame(3, 0x01, 7, 0, 0)))
    assert "tgt=7" in raw and "(" not in raw.split("tgt=7")[1][:1]

    print("test_wkt_decode: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
