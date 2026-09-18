#!/usr/bin/env python3
"""Unit tests for the workout-frame decoder used by mock_bridge.py."""
import struct
import sys

import wkt_decode as w


def frame(timer, flags, target, lo, hi, version=1, intensity=0xFF,
          dur_type=0xFF, dur_value=0, rep=0):
    """A v1 frame: 15 bytes, durationValue as a u32, no next slot."""
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


def frame2(timer, flags, target, lo, hi, version=2, intensity=0xFF,
           dur_type=0xFF, dur_value=0, remaining=0xFFFF, rep=0,
           next_intensity=0xFF, next_target=0xFF, next_mmps=0, advance=0):
    """A v2 frame: 20 bytes, durationValue narrowed to a u16 so [12..13] can
    carry remaining_s, plus the next slot and the advance in [15..19]."""
    f = bytearray(20)
    f[0] = version
    f[1] = timer
    f[2] = flags
    f[3] = intensity
    f[4] = target
    struct.pack_into("<HH", f, 5, lo, hi)
    f[9] = dur_type
    struct.pack_into("<HH", f, 10, dur_value, remaining)
    f[14] = rep
    f[15] = next_intensity
    f[16] = next_target
    struct.pack_into("<H", f, 17, next_mmps)
    f[19] = advance
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

    # v1 fields that v2 carries but v1 does not get the "says nothing" values,
    # so no caller has to branch on the version.
    assert d["remaining_s"] == w.REMAIN_UNKNOWN
    assert d["has_next"] is False and d["adv_up_only"] is False
    assert d["advance_s"] == 0 and d["next_mmps"] == 0
    assert d["next_intensity"] == 0xFF and d["next_target"] == 0xFF
    assert w.next_kmh(d) is None
    assert w.in_advance_window(d) is False

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

    # A 14-byte frame (one short of FRAME_LEN_V1) is still rejected — the
    # boundary is strictly "shorter than the version's length", matching
    # workout_ctrl_on_frame()'s `len < need`.
    try:
        w.decode(frame(3, 0x01, 0, 2222, 2500)[:14])
    except w.Malformed as e:
        assert "len=14" in str(e), str(e)
    else:
        raise AssertionError("expected Malformed for a 14-byte frame")

    # A 16-byte v1 frame (one longer than FRAME_LEN_V1) is NOT rejected: the
    # firmware accepts len >= that and acts on the frame, so the mock must
    # decode it too rather than reporting MALFORMED for something the bridge
    # would actually obey. The extra byte is still surfaced, as a note rather
    # than a rejection.
    oversized = frame(3, 0x01, 0, 2222, 2500) + b"\x00"
    d = w.decode(oversized)
    assert d["extra_bytes"] == 1
    assert d["timer"] == 3 and d["target"] == 0  # decoded from the first 15 bytes
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

    # ---- wire format v2 -------------------------------------------------
    # Every v2-only field round-trips, and the v1 fields are byte-identical
    # (0..9) so a reader of the shared prefix cannot tell the versions apart.
    d = w.decode(frame2(3, 0x01 | w.FLAG_HAS_NEXT, 0, 2361, 2361, intensity=0,
                        dur_type=0, dur_value=60, remaining=4, rep=1,
                        next_intensity=1, next_target=2, next_mmps=0,
                        advance=5))
    assert d["version"] == 2
    assert d["timer"] == 3 and d["intensity"] == 0 and d["target"] == 0
    assert d["lo_mmps"] == 2361 and d["hi_mmps"] == 2361
    assert d["dur_type"] == 0 and d["dur_value"] == 60
    assert d["remaining_s"] == 4
    assert d["rep"] == 1
    assert d["next_intensity"] == 1 and d["next_target"] == 2
    assert d["next_mmps"] == 0
    assert d["advance_s"] == 5
    assert d["has_next"] is True and d["adv_up_only"] is False
    # The next slot is a rest step with an OPEN target: it carries no speed of
    # its own, and the walk-speed fallback is policy this module does not own.
    assert w.next_kmh(d) is None
    assert w.in_advance_window(d) is True

    # A speed-targeted next slot resolves; the watch has already reduced the
    # range to a midpoint so there is nothing to average.
    d = w.decode(frame2(3, 0x01 | w.FLAG_HAS_NEXT, 0, 2361, 2361, intensity=0,
                        next_intensity=0, next_target=0, next_mmps=3333,
                        remaining=5, advance=5))
    assert abs(w.next_kmh(d) - 12.0) < 0.01
    assert abs(w.cur_kmh(d) - 8.5) < 0.01

    # FLAG_ADV_UP_ONLY is surfaced as its own field rather than left in flags.
    d = w.decode(frame2(3, 0x01 | w.FLAG_HAS_NEXT | w.FLAG_ADV_UP_ONLY, 0,
                        2361, 2361, next_target=0, next_mmps=3333,
                        remaining=5, advance=5))
    assert d["adv_up_only"] is True

    # in_advance_window() is only the frame-shaped half of the pre-roll rule.
    # Each of its four inputs vetoes on its own.
    base = dict(flags=0x01 | w.FLAG_HAS_NEXT, next_target=0, next_mmps=3333)
    assert w.in_advance_window(
        w.decode(frame2(3, target=0, lo=2361, hi=2361, remaining=5,
                        advance=5, **base))) is True
    assert w.in_advance_window(      # one second too early
        w.decode(frame2(3, target=0, lo=2361, hi=2361, remaining=6,
                        advance=5, **base))) is False
    assert w.in_advance_window(      # advance off
        w.decode(frame2(3, target=0, lo=2361, hi=2361, remaining=5,
                        advance=0, **base))) is False
    assert w.in_advance_window(      # remaining "far"
        w.decode(frame2(3, target=0, lo=2361, hi=2361, remaining=w.REMAIN_FAR,
                        advance=5, **base))) is False
    assert w.in_advance_window(      # remaining unknown
        w.decode(frame2(3, target=0, lo=2361, hi=2361,
                        remaining=w.REMAIN_UNKNOWN, advance=5, **base))) is False
    assert w.in_advance_window(      # no next step, flag clear
        w.decode(frame2(3, 0x01, 0, 2361, 2361, remaining=5, advance=5,
                        next_target=0, next_mmps=3333))) is False

    # Length is per-version: 19 bytes is short for a v2 frame even though it
    # comfortably clears the v1 minimum, and the message says which.
    try:
        w.decode(frame2(3, 0x01, 0, 2361, 2361)[:19])
    except w.Malformed as e:
        assert "len=19" in str(e) and "v2" in str(e), str(e)
    else:
        raise AssertionError("expected Malformed for a 19-byte v2 frame")

    # ...and a 21-byte v2 frame is oversized, not malformed, same as v1.
    d = w.decode(frame2(3, 0x01, 0, 2361, 2361) + b"\x00")
    assert d["extra_bytes"] == 1 and "oversized" in w.annotate(d)

    # A v3 frame is rejected, and the message names both versions the firmware
    # actually accepts rather than only the newest.
    try:
        w.decode(frame2(3, 0x01, 0, 2361, 2361, version=3))
    except w.Malformed as e:
        assert "ver=3" in str(e) and "1 or 2" in str(e), str(e)
    else:
        raise AssertionError("expected Malformed for a v3 frame")

    # The v2 line names the next slot, the remaining time and the advance --
    # the three inputs to the pre-roll decision, next to the decision itself.
    text = w.format_frame(w.decode(frame2(
        3, 0x01 | w.FLAG_HAS_NEXT, 0, 2361, 2361, intensity=0, dur_type=0,
        dur_value=60, remaining=4, next_intensity=0, next_target=0,
        next_mmps=3333, advance=5)))
    assert "next=3333 mm/s (12.0 km/h) 0(active)" in text, text
    assert "rem=4s" in text and "adv=5s" in text, text
    assert "up-only" not in text, text
    far = w.format_frame(w.decode(frame2(
        3, 0x01, 0, 2361, 2361, remaining=w.REMAIN_FAR, advance=5)))
    assert "next=none" in far and "rem=far" in far, far
    unk = w.format_frame(w.decode(frame2(3, 0x01, 0, 2361, 2361, advance=5)))
    assert "rem=unknown" in unk, unk
    up = w.format_frame(w.decode(frame2(
        3, 0x01 | w.FLAG_HAS_NEXT | w.FLAG_ADV_UP_ONLY, 0, 2361, 2361,
        next_target=0, next_mmps=3333, remaining=2, advance=5)))
    assert "up-only" in up, up

    # A v1 frame gets no third line at all -- there is nothing to say.
    assert w.format_frame(w.decode(frame(3, 0x01, 0, 2222, 2500))).count("\n") == 1

    print("test_wkt_decode: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
