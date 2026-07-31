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

    # Content annotations describe the frame, not the belt policy.
    assert "FREE RUN" in w.annotate(w.decode(frame(3, 0x00, 0xFF, 0, 0)))
    assert w.annotate(w.decode(frame(2, 0x00, 0xFF, 0, 0))) == "timer not running"
    assert w.annotate(w.decode(frame(3, 0x01, 2, 0, 0))) == "non-speed target"
    assert w.annotate(w.decode(frame(3, 0x01, 0, 2222, 2500))) == "speed step"

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
