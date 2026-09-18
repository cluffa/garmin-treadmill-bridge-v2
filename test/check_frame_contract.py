#!/usr/bin/env python3
"""Assert the workout-frame wire format agrees across C, Monkey C and Python.

The A6ED0004 frame has three independent implementations: the bridge decodes it
(core/workout_ctrl.c, constants in core/workout_ctrl.h), the Garmin data field
packs it (watch/garmin_data_field/source/DataFieldView.mc) and the mock bridge
reads it back (test/mock/wkt_decode.py). Nothing links them, so the version
byte and the frame length are exactly the kind of three-way constant that
drifts when one side is edited alone -- and the failure is silent in the worst
way: the firmware drops the frame as MALFORMED and the belt simply never moves,
which this project has already spent sessions misreading as a BLE problem.

What must hold:

  * the bridge and the mock decoder agree on *both* accepted versions and their
    lengths. They are two sides of one decision (`workout_ctrl_on_frame()` and
    `wkt_decode.decode()`), so anything less than equality is drift.
  * the data field's (FRAME_VERSION, FRAME_LEN) is one of the pairs the bridge
    accepts. Deliberately not equality: an older watch build sending v1 frames
    to new firmware is a supported configuration, and the whole reason v1 is
    still accepted. What is never supported is the watch declaring a version
    the firmware has never heard of, or the right version at the wrong length.

Run: python3 test/check_frame_contract.py   (exit 0 = agree, 1 = drift)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "core" / "workout_ctrl.h"
DECODER = ROOT / "test" / "mock" / "wkt_decode.py"
DATAFIELD = ROOT / "watch" / "garmin_data_field" / "source" / "DataFieldView.mc"


def grab(path: Path, pattern: str, what: str) -> int:
    """The single integer `pattern` captures in `path`, or a hard failure.

    A missing constant is a failure, not a skip: it means the file was
    renamed, rewritten or had the constant folded away, and this guard has
    quietly stopped watching it.
    """
    if not path.exists():
        sys.exit(f"FAIL: {path.relative_to(ROOT)} does not exist")
    found = re.findall(pattern, path.read_text(), re.M)
    if len(found) != 1:
        sys.exit(f"FAIL: found {len(found)} definitions of {what} in "
                 f"{path.relative_to(ROOT)}, expected exactly 1")
    return int(found[0])


def main() -> int:
    c_ver = grab(HEADER, r"#define\s+WORKOUT_FRAME_VERSION\s+(\d+)",
                 "WORKOUT_FRAME_VERSION")
    c_len = grab(HEADER, r"#define\s+WORKOUT_FRAME_LEN\s+(\d+)",
                 "WORKOUT_FRAME_LEN")
    c_len_v1 = grab(HEADER, r"#define\s+WORKOUT_FRAME_LEN_V1\s+(\d+)",
                    "WORKOUT_FRAME_LEN_V1")

    py_ver = grab(DECODER, r"^FRAME_VERSION\s*=\s*(\d+)$", "FRAME_VERSION")
    py_len = grab(DECODER, r"^FRAME_LEN\s*=\s*(\d+)$", "FRAME_LEN")
    py_ver_v1 = grab(DECODER, r"^FRAME_VERSION_V1\s*=\s*(\d+)$",
                     "FRAME_VERSION_V1")
    py_len_v1 = grab(DECODER, r"^FRAME_LEN_V1\s*=\s*(\d+)$", "FRAME_LEN_V1")

    mc_ver = grab(DATAFIELD, r"FRAME_VERSION\s*=\s*(\d+)\s*;", "FRAME_VERSION")
    mc_len = grab(DATAFIELD, r"FRAME_LEN\s*=\s*(\d+)\s*;", "FRAME_LEN")

    # workout_ctrl_on_frame() accepts version 1 at WORKOUT_FRAME_LEN_V1 and the
    # current version at WORKOUT_FRAME_LEN.
    accepted = {(1, c_len_v1), (c_ver, c_len)}

    print(f"bridge  ({HEADER.relative_to(ROOT)}): "
          f"v{c_ver} len {c_len}, v1 len {c_len_v1}")
    print(f"decoder ({DECODER.relative_to(ROOT)}): "
          f"v{py_ver} len {py_len}, v{py_ver_v1} len {py_len_v1}")
    print(f"watch   ({DATAFIELD.relative_to(ROOT)}): v{mc_ver} len {mc_len}")

    bad = []
    if (py_ver, py_len) != (c_ver, c_len) or (py_ver_v1, py_len_v1) != (1, c_len_v1):
        bad.append(
            f"{DECODER.relative_to(ROOT)} decodes v{py_ver}/{py_len} and "
            f"v{py_ver_v1}/{py_len_v1}, but the bridge accepts "
            f"{sorted(accepted)} -- the mock would report MALFORMED for frames "
            "the firmware obeys, or obey frames it drops")
    if (mc_ver, mc_len) not in accepted:
        bad.append(
            f"{DATAFIELD.relative_to(ROOT)} packs v{mc_ver} at {mc_len} bytes, "
            f"which the bridge does not accept (it takes {sorted(accepted)}) -- "
            "every frame would be dropped as MALFORMED and the belt would "
            "never move, with nothing in the log on the watch to say why")

    if bad:
        print("\ncheck_frame_contract: FAIL")
        for b in bad:
            print(f"  {b}")
        return 1

    print("\ncheck_frame_contract: OK -- bridge, watch and mock agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
