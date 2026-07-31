#!/usr/bin/env python3
"""Assert every side of the A6ED control contract agrees on the 128-bit base.

Why this exists: v2 once shipped the base as a sanitization placeholder
(`A6ED0000-2E7A-4E1D-9E3B-000000000000`) while the Garmin CIQ apps used the real
`A6ED0001-D344-460A-8075-B9E8EC90D71B`. The CIQ apps discover the bridge by
*filtering on the 128-bit service UUID*, so the mismatch made the bridge
completely invisible to the watch — with no error anywhere to explain it. It went
unnoticed because the firmware and the watch code lived in different repos, so
nothing could observe them diverging.

The firmware is the source of truth. It stores the base as little-endian bytes
with a 16-bit alias placeholder at bytes 12-13, so this reverses them back to a
UUID string and compares every other participant against it. It also checks the
16-bit *alias* half of each UUID a consumer uses (the `0001`/`0002`/`0003`/`0004`
that picks the characteristic) against the aliases the firmware actually defines
— a consumer that agrees on the 96-bit base but references a characteristic the
firmware doesn't have is just as silently broken as one on the wrong base. And a
consumer that matches zero A6ED UUIDs at all is treated as a failure, not a
warning: a file in this list that references none of them has either been
renamed/rewritten wholesale or never had the contract in it to begin with, and
either way the guard has stopped watching it.

Run: python3 test/check_uuid_contract.py   (exit 0 = agree, 1 = drift)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIRMWARE = ROOT / "firmware" / "ble_ctrl_svc.c"

# Everything that must agree with the firmware, and how to find UUIDs in it.
CONSUMERS = [
    ROOT / "test" / "mock" / "mock_watch.py",
    ROOT / "test" / "mock" / "mock_bridge.py",
    ROOT / "watch" / "garmin_data_field" / "source" / "CtrlBleDelegate.mc",
    ROOT / "watch" / "garmin_ctrl_app" / "source" / "BridgeBle.mc",
]

UUID_RE = re.compile(r"A6ED([0-9A-F]{4})-([0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{4}-[0-9A-F]{12})",
                     re.IGNORECASE)


def firmware_base() -> str:
    """Reverse the little-endian ble_uuid128_t into a canonical UUID string."""
    src = FIRMWARE.read_text()
    m = re.search(r"CTRL_BASE\s*=\s*\{\{(.*?)\}\}", src, re.S)
    if not m:
        sys.exit(f"FAIL: could not find CTRL_BASE in {FIRMWARE}")
    octets = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))]
    if len(octets) != 16:
        sys.exit(f"FAIL: CTRL_BASE has {len(octets)} bytes, expected 16")
    be = bytes(reversed(octets)).hex().upper()
    # Bytes 12-13 little-endian == string chars 8..12 are the 16-bit alias slot.
    return f"{be[0:8]}-{be[8:12]}-{be[12:16]}-{be[16:20]}-{be[20:32]}"


def firmware_aliases() -> set:
    """The 16-bit characteristic/service aliases the firmware actually defines,
    e.g. {"0001", "0002", "0003", "0004"} from CTRL_SVC_UUID / CTRL_CHR_UUID /
    CTRL_RSP_UUID / CTRL_WKT_UUID. Derived from source, not hardcoded, so a
    firmware-side rename is what it should be: a drift the guard catches rather
    than a constant this script has to be kept in sync with by hand.
    """
    src = FIRMWARE.read_text()
    found = re.findall(r"#define\s+CTRL_\w+_UUID\s+0x([0-9a-fA-F]{4})\b", src)
    if not found:
        sys.exit(f"FAIL: could not find any CTRL_*_UUID defines in {FIRMWARE}")
    return {a.upper() for a in found}


def main() -> int:
    base = firmware_base()
    suffix = base.split("-", 1)[1]          # everything after A6ED0000
    aliases = firmware_aliases()
    print(f"firmware base: {base}  ({FIRMWARE.relative_to(ROOT)})")
    print(f"firmware characteristic aliases: {', '.join(sorted(aliases))}")

    if suffix.strip("0-") == "":
        print("\nFAIL: the firmware base looks like a sanitization placeholder "
              "(all-zero tail).")
        print("The CIQ apps filter on this UUID; a placeholder makes the bridge "
              "invisible to the watch.")
        return 1

    bad = 0
    for path in CONSUMERS:
        if not path.exists():
            print(f"  SKIP {path.relative_to(ROOT)} (absent)")
            continue
        found = UUID_RE.findall(path.read_text())
        if not found:
            # A listed consumer that matches nothing has either been renamed,
            # rewritten wholesale, or lost its A6ED references entirely — that
            # is exactly the kind of drift this guard exists to catch, not
            # something to shrug off as a warning.
            bad += 1
            print(f"  FAIL {path.relative_to(ROOT)}: no A6ED UUIDs found — "
                  "misspelled or drifted wholesale")
            continue

        wrong_base = sorted({s.upper() for _, s in found} - {suffix})
        unknown_alias = sorted({a.upper() for a, _ in found} - aliases)
        if wrong_base or unknown_alias:
            bad += 1
            reasons = []
            if wrong_base:
                reasons.append(f"wrong base {', '.join(wrong_base)}")
            if unknown_alias:
                reasons.append(
                    f"characteristic alias not defined by the firmware "
                    f"{', '.join(unknown_alias)}")
            print(f"  FAIL {path.relative_to(ROOT)}: {'; '.join(reasons)}")
        else:
            print(f"  ok   {path.relative_to(ROOT)} ({len(found)} UUIDs)")

    if bad:
        print(f"\ncheck_uuid_contract: FAIL — {bad} file(s) disagree with the "
              "firmware.")
        print("The watch discovers the bridge by filtering on this UUID, and "
              "picks a characteristic by its 16-bit alias. Either kind of "
              "mismatch is silent:")
        print("the watch simply never finds the device, or writes to a "
              "characteristic the firmware never defined.")
        return 1

    print("\ncheck_uuid_contract: OK — all participants agree.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
