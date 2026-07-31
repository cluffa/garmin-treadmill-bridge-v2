# Mock Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a macOS BLE peripheral that impersonates the nRF52840 bridge so the real Garmin watch data field can be debugged without the hardware bridge in the loop.

**Architecture:** A `bless` (CoreBluetooth) peripheral advertises the `A6ED0001` control service and exposes chars `0002`/`0003`/`0004`. Writes to `0004` are decoded by a pure-Python module and fed to the **actual** `core/workout_ctrl.c`, compiled to a shared library and loaded through ctypes, so the predicted belt action can never drift from firmware. Everything is reported as a timestamped scrolling log.

**Tech Stack:** Python 3.9+, `bless` (declared inline via the `uv run --script` header, as in `test/mock/mock_treadmill.py`), host `cc` for the shared library, ctypes. No nRF SDK, no ARM toolchain, no pytest.

**Spec:** `docs/superpowers/specs/2026-07-30-mock-bridge-design.md`

## Global Constraints

- Service UUID is exactly `A6ED0001-D344-460A-8075-B9E8EC90D71B`. Never substitute a placeholder — the watch discovers the bridge by *filtering* on this UUID, so a mismatch is silent (`watch/README.md`).
- `await server.start(prioritize_local_name=False)` is **required**, not a preference. `bless` drops every service UUID from the advertisement when `prioritize_local_name` is true and the local name is longer than 10 characters. See the comment at `test/mock/mock_treadmill.py:106-113`.
- `core/` purity invariant: nothing new goes in `core/`. The C shim lives in `test/mock/`.
- The shared library is named `libworkout_probe.so` (not `.dylib`) — `ctypes.CDLL` loads a `-shared` output named `.so` fine on macOS, and the single name keeps the Makefile portable.
- Wire format is frozen for this plan: 15 bytes, version 1, layout per `core/workout_ctrl.h:20-35`. No new fields.
- No pytest. Tests are plain scripts using `assert`, matching the C-assert style already in `test/host/`.
- Every Python test must be runnable without `bless` installed. Keep BLE imports out of the modules under test.

---

### Task 1: Advertising skeleton (hardware gate)

This is the load-bearing task. If CoreBluetooth will not put the 128-bit UUID where `ScanResult.getServiceUuids()` can see it, the Python approach is dead and the fallback is a Swift `CBPeripheralManager` tool. Nothing else is worth building until the watch connects.

**Files:**
- Create: `test/mock/mock_bridge.py`

**Interfaces:**
- Consumes: nothing.
- Produces: `log(tag: str, msg: str) -> None` and the module-level UUID constants `CTRL_SVC`, `CTRL_CHR`, `CTRL_RSP`, `CTRL_WKT`, used by Task 4.

- [ ] **Step 1: Write the script**

```python
#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bless"]
# ///
"""Mock nRF52840 bridge: a macOS BLE peripheral for debugging the watch data field.

Advertises the A6ED control service so watch/garmin_data_field connects to this
Mac instead of the hardware bridge, and logs the 15-byte workout telemetry
frames it writes to A6ED0004.

Run: make mock-bridge   (from the repo root)

⚠ Power the real bridge OFF first. The data field pairs with the first device it
finds advertising the service UUID; with both on air you will debug the wrong peer.
"""

import asyncio
import time
from datetime import datetime

from bless import (
    BlessServer,
    GATTCharacteristicProperties as Props,
    GATTAttributePermissions as Perm,
)

# Must match firmware/ble_ctrl_svc.c. test/check_uuid_contract.py enforces this.
CTRL_SVC = "A6ED0001-D344-460A-8075-B9E8EC90D71B"
CTRL_CHR = "A6ED0002-D344-460A-8075-B9E8EC90D71B"  # write (ctrl grammar)
CTRL_RSP = "A6ED0003-D344-460A-8075-B9E8EC90D71B"  # notify (D/E/S frames)
CTRL_WKT = "A6ED0004-D344-460A-8075-B9E8EC90D71B"  # write (workout telemetry)

# <= 10 chars: bless drops service UUIDs from the advert for longer names when
# prioritize_local_name is true. We pass False anyway, but a short name keeps
# this correct under either setting. "MOCK" distinguishes it from the real
# bridge in a phone scanner.
ADV_NAME = "TMILL-MOCK"

IDLE_LOG_S = 10.0

# Written from the CoreBluetooth callback thread, read from the asyncio loop.
# A bare float assignment is atomic under the GIL, so no lock is needed.
_last_event = time.monotonic()


def log(tag: str, msg: str) -> None:
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{ts}  {tag:<5} {msg}", flush=True)


def on_write(characteristic, value: bytearray) -> None:
    """bless dispatches every characteristic write here."""
    global _last_event
    _last_event = time.monotonic()
    uuid = str(characteristic.uuid).upper()
    if uuid.startswith("A6ED0004"):
        log("WKT", f"len={len(value)} raw={bytes(value).hex()}")
    else:
        log("CTRL", f"{uuid[:8]} {bytes(value).hex()}")


async def main() -> None:
    global _last_event

    server = BlessServer(name=ADV_NAME)
    server.read_request_func = None
    server.write_request_func = on_write

    await server.add_new_service(CTRL_SVC)
    # write + write_without_response mirrors firmware/ble_ctrl_svc.c:482,532.
    # CtrlBleDelegate uses WRITE_TYPE_DEFAULT (write *with* response), so plain
    # `write` is the one that actually matters here.
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_CHR,
        Props.write | Props.write_without_response, None,
        Perm.readable | Perm.writeable,
    )
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_RSP,
        Props.notify, None, Perm.readable,
    )
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_WKT,
        Props.write | Props.write_without_response, None,
        Perm.readable | Perm.writeable,
    )

    # prioritize_local_name=False is REQUIRED, not a preference. bless defaults
    # it to True, and with it set it drops every service UUID from the
    # advertisement when the local name is longer than 10 characters (bless
    # corebluetooth server.py: `if (prioritize_local_name) and len(self.name) >
    # 10`). The data field filters scan results on the 128-bit service UUID and
    # nothing else, so an advert without it is invisible to the watch — with no
    # error anywhere to explain it.
    await server.start(prioritize_local_name=False)
    log("ADV", f"{ADV_NAME}  {CTRL_SVC}")
    log("ADV", "power the real bridge OFF — the field pairs with whatever it finds first")

    connected = False
    try:
        while True:
            await asyncio.sleep(1.0)
            now = await server.is_connected()
            if now != connected:
                connected = now
                _last_event = time.monotonic()
                log("LINK", "central connected" if now else "central disconnected")
                continue
            if connected and time.monotonic() - _last_event >= IDLE_LOG_S:
                _last_event = time.monotonic()
                log("idle", "(no write — field sends on change only)")
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        await server.stop()
        log("ADV", "stopped")


if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 2: Make it executable and run it**

```bash
chmod +x test/mock/mock_bridge.py
cd test/mock && ./mock_bridge.py
```

Expected: an `ADV` line within a few seconds. If macOS has not been granted Bluetooth permission for the terminal, this hangs — grant it in System Settings → Privacy & Security → Bluetooth.

- [ ] **Step 3: THE GATE — confirm the real watch connects**

With the mock running and **the hardware bridge powered off**:
1. On the watch, start a run activity that has the bridge data field on a screen.
2. Watch the field's bottom line: it shows `SCAN`, then must change to `CONN`.
3. The mock must log `LINK  central connected`.

Expected: `CONN` on the watch and a `LINK` line in the log.

**If it fails**, disambiguate before changing any code — install nRF Connect or LightBlue on a phone and scan for `TMILL-MOCK`:
- *Not visible at all* → advertising itself is broken; check Bluetooth permission and the `bless` start call.
- *Visible but no service UUID listed in the advert* → this is the risk-1 failure. Python is out; **stop and escalate** — the fallback is a Swift `CBPeripheralManager` tool (spec approach C), which is a different plan.
- *Visible with the UUID, but the watch still will not pair* → risk 4 (pairing/bonding). Escalate with the field's console output.

- [ ] **Step 4: Commit**

```bash
git add test/mock/mock_bridge.py
git commit -m "test(mock): macOS BLE peripheral that impersonates the bridge

Advertises the A6ED control service so the Garmin data field connects to a
Mac instead of the hardware bridge. This commit is the advertising skeleton
only: it logs link state and raw writes, no decoding yet."
```

---

### Task 2: `workout_probe.c` — the real policy behind a ctypes ABI

**Files:**
- Create: `test/mock/workout_probe.c`
- Create: `test/mock/Makefile`
- Test: `test/mock/test_workout_probe.py`

**Interfaces:**
- Consumes: `core/workout_ctrl.c`, `core/workout_ctrl.h`, `core/machine.h`.
- Produces: `libworkout_probe.so` exporting
  `void probe_reset(void)`,
  `int probe_feed(const uint8_t *buf, uint16_t len)`,
  `int probe_tick(void)`,
  `float probe_last_speed(void)`.
  Action codes: `0` = none issued, `1` = speed, `2` = stop. Used by Task 4.

- [ ] **Step 1: Write the failing test**

Create `test/mock/test_workout_probe.py`:

```python
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `python3 test/mock/test_workout_probe.py`
Expected: FAIL — `OSError` / `could not load libworkout_probe.so` (the file does not exist yet).

- [ ] **Step 3: Write the C shim**

Create `test/mock/workout_probe.c`:

```c
/*
 * workout_probe.c — expose core/workout_ctrl.c to mock_bridge.py via ctypes.
 *
 * The mock bridge reports what the *real* bridge would do with each frame the
 * watch sends. That only means anything if the policy is the firmware's own, so
 * this file supplies the machine_* stubs workout_ctrl.c links against, records
 * which command it issued, and exposes a scalars-only ABI (no structs, so there
 * is no layout to keep in sync with the Python side).
 *
 * Lives in test/mock/, not core/ — the core/ purity invariant is untouched.
 */
#include <stdbool.h>
#include <stddef.h>   /* NULL — not guaranteed by stdbool.h/stdint.h on Apple clang */
#include <stdint.h>

#include "machine.h"
#include "workout_ctrl.h"

#define PROBE_ACT_NONE  0
#define PROBE_ACT_SPEED 1
#define PROBE_ACT_STOP  2

static int   g_act;
static float g_last_speed;

/* ---- machine_* stubs: record what workout_ctrl commanded ---- */
void  machine_start_scan(void) {}
int   machine_get_devices(ftms_device_t *o, int m) { (void)o; (void)m; return 0; }
void  machine_connect(const ftms_device_t *d) { (void)d; }
bool  machine_connected(void) { return true; }
const ftms_device_t *machine_connected_device(void) { return NULL; }
bool  machine_set_incline(float p) { (void)p; return true; }
bool  machine_set_speed(float kmh) { g_act = PROBE_ACT_SPEED; g_last_speed = kmh; return true; }
bool  machine_stop(void) { g_act = PROBE_ACT_STOP; return true; }

/* ---- probe ABI ---- */

void probe_reset(void)
{
    workout_ctrl_reset();
    g_act = PROBE_ACT_NONE;
    g_last_speed = 0.0f;
}

/* Feed one frame. Returns the command actually issued to the machine, so a
 * deduplicated frame correctly reports PROBE_ACT_NONE. */
int probe_feed(const uint8_t *buf, uint16_t len)
{
    g_act = PROBE_ACT_NONE;
    workout_ctrl_on_frame(buf, len);
    return g_act;
}

/* One 1 Hz tick. Returns PROBE_ACT_SPEED when the keepalive re-asserted. */
int probe_tick(void)
{
    g_act = PROBE_ACT_NONE;
    workout_ctrl_tick();
    return g_act;
}

float probe_last_speed(void) { return g_last_speed; }
```

Create `test/mock/Makefile`:

```make
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
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -C test/mock test`
Expected: `test_workout_probe: OK`, exit 0. No compiler warnings.

- [ ] **Step 5: Commit**

```bash
git add test/mock/workout_probe.c test/mock/Makefile test/mock/test_workout_probe.py
git commit -m "test(mock): expose core/workout_ctrl.c to the mock via ctypes

A scalars-only shim plus machine_* stubs, built as libworkout_probe.so, so
the mock bridge can report the firmware's real belt action for each frame
rather than a Python reimplementation of decode_action()."
```

---

### Task 3: `wkt_decode.py` — frame decoding and log formatting

Kept separate from `mock_bridge.py` so it is testable without `bless` installed.

**Files:**
- Create: `test/mock/wkt_decode.py`
- Test: `test/mock/test_wkt_decode.py`
- Modify: `test/mock/Makefile` (add the new test to the `test` target)

**Interfaces:**
- Consumes: nothing.
- Produces, for Task 4:
  - `FRAME_LEN: int = 15`, `FRAME_VERSION: int = 1`
  - `class Malformed(Exception)`
  - `decode(buf: bytes) -> dict` — raises `Malformed`; keys `version, timer, flags, intensity, target, lo_mmps, hi_mmps, dur_type, dur_value, rep`
  - `kmh(mmps: int) -> float`
  - `format_frame(d: dict) -> str` — two lines, second already indented
  - `annotate(d: dict) -> str`
  - `INDENT: str` — the 20-space continuation indent that lines up under `log()`'s message column

- [ ] **Step 1: Write the failing test**

Create `test/mock/test_wkt_decode.py`:

```python
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
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd test/mock && python3 test_wkt_decode.py`
Expected: FAIL — `ModuleNotFoundError: No module named 'wkt_decode'`.

- [ ] **Step 3: Write the module**

Create `test/mock/wkt_decode.py`:

```python
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
```

- [ ] **Step 4: Add it to the Makefile test target**

In `test/mock/Makefile`, replace the `test` target with:

```make
test: $(LIB)
	python3 test_workout_probe.py
	python3 test_wkt_decode.py
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `make -C test/mock test`
Expected: `test_workout_probe: OK` then `test_wkt_decode: OK`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add test/mock/wkt_decode.py test/mock/test_wkt_decode.py test/mock/Makefile
git commit -m "test(mock): decode and format the 15-byte workout frame

Reading only — no belt policy lives here, so there stays exactly one
implementation of that decision (core/workout_ctrl.c). Kept free of BLE
imports so it is testable without bless installed."
```

---

### Task 4: Wire decoding and predicted action into the mock

**Files:**
- Modify: `test/mock/mock_bridge.py`

**Interfaces:**
- Consumes: `wkt_decode` (Task 3), `libworkout_probe.so` (Task 2), `log()` and the UUID constants (Task 1).
- Produces: the finished mock. Nothing downstream imports it.

- [ ] **Step 1: Add the probe binding**

In `test/mock/mock_bridge.py`, add to the imports:

```python
import ctypes
import pathlib
import threading

import wkt_decode as wkt
```

and below the constants:

```python
LIB = pathlib.Path(__file__).with_name("libworkout_probe.so")
ACT = {0: "no change", 1: "ACT_SPEED", 2: "ACT_STOP"}

# workout_ctrl.c holds static state, and it is reached from two threads: the
# CoreBluetooth callback thread (writes) and the asyncio loop (1 Hz tick).
_probe_lock = threading.Lock()


def load_probe():
    if not LIB.exists():
        raise SystemExit(
            f"{LIB.name} is missing. Build it first:\n"
            f"    make -C {LIB.parent}\n"
            f"or just run `make mock-bridge` from the repo root."
        )
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


probe = None      # set in main()
frame_no = 0      # written only from the CoreBluetooth callback thread
```

- [ ] **Step 2: Replace `on_write` with the decoding version**

```python
def _action_str(act: int) -> str:
    if act == 1:
        return f"-> ACT_SPEED {probe.probe_last_speed():.1f} km/h"
    if act == 2:
        return "-> ACT_STOP"
    return "-> no change (deduplicated, or belt held)"


def on_write(characteristic, value: bytearray) -> None:
    """bless dispatches every characteristic write here."""
    global _last_write, _last_log, frame_no
    _last_write = _last_log = time.monotonic()

    uuid = str(characteristic.uuid).upper()
    if not uuid.startswith("A6ED0004"):
        # 0002 exists so service discovery matches the firmware's table; the
        # ctrl grammar is deliberately not emulated. Log and move on.
        log("CTRL", f"{uuid[:8]} {bytes(value).hex()}  (ignored)")
        return

    frame_no += 1
    raw = bytes(value)
    try:
        d = wkt.decode(raw)
    except wkt.Malformed as e:
        # workout_ctrl_on_frame() drops these silently; the mock must not.
        log("WKT", f"#{frame_no}  {e}  MALFORMED - dropped  raw={raw.hex()}")
        return

    with _probe_lock:
        act = probe.probe_feed(raw, len(raw))
        detail = _action_str(act)

    # One log() call, so every continuation line keeps the same indent.
    log("WKT", f"#{frame_no}  len={len(raw)} {wkt.format_frame(d)}\n"
               f"{wkt.INDENT}{detail}   [{wkt.annotate(d)}]")
```

- [ ] **Step 3: Load the probe and tick it from `main()`**

At the top of `main()`, before constructing the server:

```python
    global probe, _last_write, _last_log
    probe = load_probe()
    probe.probe_reset()
```

`_last_write` and `_last_log` must be in that `global` statement: both are assigned inside the loop, and without it Python treats them as locals and raises `UnboundLocalError` on the first iteration.

and **replace the whole `connected` / `is_connected()` link-state mechanism from Task 1**, which the hardware gate proved cannot work here.

> **Why this changed.** The 2026-07-30 gate run connected a real watch and received
> four frames, but no `LINK` line ever printed. `bless`'s `is_connected()` on the
> CoreBluetooth backend reports *subscribed centrals*, and the data field
> deliberately never subscribes to `A6ED0003` (`watch/README.md`) — so it is
> structurally always `False` for this peer. The same run showed a legitimate
> **35-second gap** between frames while connected, because the field writes only
> on change, so a short write-gap timeout would produce false disconnects. Link
> state is therefore *inferred* from traffic on a generous window, and the log says
> so rather than claiming a connection it cannot observe.

Replace the `connected = False` initialiser above the loop with:

```python
    linked = False
```

and replace the body of the `while True:` loop with:

```python
            await asyncio.sleep(1.0)
            now = time.monotonic()

            # The firmware calls workout_ctrl_tick() at ~1 Hz; matching that here
            # makes the ~30 s keepalive re-assert show up exactly as it would on
            # hardware. It runs whether or not a watch is attached, because the
            # latched command is what the keepalive re-asserts.
            with _probe_lock:
                act = probe.probe_tick()
                speed = probe.probe_last_speed()
            if act == 1:
                _last_log = now
                log("KEEP", f"-> re-assert {speed:.1f} km/h")
                continue

            if _last_write is not None and not linked:
                linked = True
                _last_log = now
                log("LINK", "frames arriving - watch attached (inferred from "
                            "traffic; bless is_connected() is subscription-based "
                            "and the data field never subscribes)")
                continue

            if linked and now - _last_write >= STALE_S:
                linked = False
                # Clearing _last_write is load-bearing, not tidiness: leaving it
                # set sends the next iteration straight back into the "frames
                # arriving" branch above, and the two LINK lines then oscillate
                # at 1 Hz with a probe_reset() every ~2 s — which would drop the
                # latched keepalive right through a long free run.
                _last_write = None
                # Drop the latched command so a stale keepalive does not survive
                # into the next session.
                with _probe_lock:
                    probe.probe_reset()
                _last_log = now
                log("LINK", f"no frames for {STALE_S:.0f}s - assuming the watch "
                            f"is gone; probe reset")
                continue

            if now - _last_log >= (IDLE_LOG_S if linked else WAIT_LOG_S):
                _last_log = now
                if linked:
                    log("idle", "(no write - field sends on change only)")
                else:
                    log("wait", "no frames yet - start a run activity with the "
                                "data field on a screen")
```

This needs three module-level values in place of Task 1's single `_last_event`. Replace that definition with:

```python
# Written from the CoreBluetooth callback thread, read from the asyncio loop.
# Bare assignments to a float/None are atomic under the GIL, so no lock is needed.
_last_write = None      # monotonic time of the last characteristic write, ever
_last_log = time.monotonic()   # monotonic time of the last line the loop printed

IDLE_LOG_S = 10.0       # heartbeat cadence while frames are flowing
WAIT_LOG_S = 30.0       # quieter heartbeat before the watch ever appears
# A steady free run legitimately goes minutes without a write (the field sends on
# change), and the gate run showed a 35 s gap while plainly connected. This window
# is deliberately far larger than that — it exists to clear the latched command
# between sessions, not to track the link precisely.
STALE_S = 120.0
```

and delete Task 1's `IDLE_LOG_S = 10.0` and `_last_event` lines so there is one definition of each. Every `_last_event = time.monotonic()` in `on_write` becomes:

```python
    global _last_write, _last_log
    _last_write = _last_log = time.monotonic()
```

- [ ] **Step 4: Verify it starts and reports a missing library cleanly**

```bash
make -C test/mock clean
cd test/mock && ./mock_bridge.py
```

Expected: exits immediately with the `libworkout_probe.so is missing` message and the build command. It must **not** fall back to a Python reimplementation.

Then rebuild and start it for real:

```bash
make -C test/mock
cd test/mock && ./mock_bridge.py
```

Expected: `ADV` lines, then a `wait` heartbeat every 30 s until a watch appears. Leave it running for Task 6.

- [ ] **Step 5: Verify the link-state logic without a watch**

The link inference is the part the hardware gate proved the plan had wrong, so exercise it directly rather than trusting it. With the mock running, in another terminal call `on_write` by hand is not possible — instead temporarily set `STALE_S = 5.0` and `WAIT_LOG_S = 2.0` in a scratch copy, feed one synthetic frame through `on_write` by importing the module, and confirm the sequence `LINK frames arriving` → `idle` → `LINK no frames … probe reset`. Restore the real values afterwards. Report what you observed; do not commit the scratch values.

- [ ] **Step 6: Commit**

```bash
git add test/mock/mock_bridge.py
git commit -m "test(mock): decode 0004 writes and report the real belt action

Each frame is decoded, fed to core/workout_ctrl.c through the probe, and
logged with the command the firmware would actually have issued. A 1 Hz tick
surfaces the ~30 s keepalive.

Link state is inferred from write traffic, not from bless's is_connected():
the 2026-07-30 gate run showed that reports subscribed centrals, and the data
field never subscribes to A6ED0003, so it is always False for this peer."
```

---

### Task 5: Wire into the build gate and document

**Files:**
- Modify: `test/check_uuid_contract.py:26-30`
- Modify: `Makefile:9-10,23-45,58-60`
- Modify: `watch/README.md` (append a section)

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces: `make mock-bridge`, `make mock-test`. Nothing downstream.

- [ ] **Step 1: Add the mock to the UUID contract**

In `test/check_uuid_contract.py`, change `CONSUMERS` to:

```python
CONSUMERS = [
    ROOT / "test" / "mock" / "mock_watch.py",
    ROOT / "test" / "mock" / "mock_bridge.py",
    ROOT / "watch" / "garmin_data_field" / "source" / "CtrlBleDelegate.mc",
    ROOT / "watch" / "garmin_ctrl_app" / "source" / "BridgeBle.mc",
]
```

- [ ] **Step 2: Verify the contract check picks it up**

Run: `make check-uuid`
Expected: a new `ok   test/mock/mock_bridge.py (4 UUIDs)` line, and `check_uuid_contract: OK` overall.

- [ ] **Step 3: Add the Makefile targets**

In the top-level `Makefile`, add `mock-bridge mock-test` to the `.PHONY` list, add these targets after `host-test`:

```make
# Mock bridge: a macOS BLE peripheral that impersonates this firmware so the
# Garmin data field can be debugged without the hardware in the loop.
# See docs/superpowers/specs/2026-07-30-mock-bridge-design.md.
mock-test:
	$(MAKE) -C test/mock test

mock-bridge:
	$(MAKE) -C test/mock
	cd test/mock && ./mock_bridge.py
```

change `host-test` to run the mock's tests too:

```make
host-test: check-uuid
	$(MAKE) -C test/host
	$(MAKE) -C test/mock test
```

add to `clean`:

```make
	$(MAKE) -C test/mock clean
```

and add to the `help` text, under `Build:`:

```make
	@echo "  make mock-test              mock-bridge unit tests (decoder + probe ABI)"
	@echo "  make mock-bridge            run the macOS mock bridge (debug the watch data field)"
```

- [ ] **Step 4: Verify the full gate passes**

Run: `make host-test`
Expected: `check_uuid_contract: OK`, all nine `test/host` binaries pass, then `test_workout_probe: OK` and `test_wkt_decode: OK`. Exit 0.

- [ ] **Step 5: Document it in `watch/README.md`**

Append this section, after "Sideloading":

```markdown
## Debugging the data field without the hardware bridge

`make mock-bridge` runs a macOS BLE peripheral (`test/mock/mock_bridge.py`) that
advertises the same `A6ED0001` control service as the firmware. The data field
cannot tell it apart from the real bridge, so you can iterate on watch code with
the nRF52840 out of the loop entirely.

It logs every frame written to `A6ED0004`, decoded, along with the belt command
the firmware would actually have issued — that prediction comes from
`core/workout_ctrl.c` itself, compiled to `libworkout_probe.so` and loaded
through ctypes, so it cannot drift from the bridge.

```
21:04:31.882  LINK  central connected
21:04:33.104  WKT   #1  len=15 ver=1 timer=3(ON) flags=0x01 intensity=0(active)
                        tgt=0(SPEED) lo=2222 hi=2500 mm/s (8.0-9.0 km/h) dur=5 300 rep=0
                        -> ACT_SPEED 8.5 km/h   [speed step]
21:05:03.900  KEEP  -> re-assert 8.5 km/h
```

⚠ **Power the real bridge off first.** The data field pairs with the first
device it finds advertising the service UUID; with both on air you will be
debugging the wrong peer.

A quiet log is usually correct — the field only writes when the frame changes,
which is why an `idle` heartbeat prints every 10 s while connected.

It does **not** emulate the ctrl grammar (`A6ED0002`/`0003`), so
`garmin_ctrl_app` is not exercised by it.
```

- [ ] **Step 6: Commit**

```bash
git add test/check_uuid_contract.py Makefile watch/README.md
git commit -m "build: gate the mock bridge and document it

Adds mock_bridge.py to the UUID contract check so it cannot silently drift
from the firmware base, runs its unit tests as part of make host-test, and
adds make mock-bridge."
```

---

### Task 6: Hardware bring-up

**Files:**
- Create: `docs/superpowers/test-logs/2026-07-30-mock-bridge-bringup.md`

**Interfaces:**
- Consumes: the finished mock.
- Produces: a record of what the real watch actually did.

- [ ] **Step 1: Run the acceptance sequence**

With the hardware bridge **powered off**, `make mock-bridge` running, and the data field on a run activity screen:

| # | Action on the watch | Expected in the log |
|---|---|---|
| 1 | Start a run activity | `LINK  central connected`, field shows `CONN` |
| 2 | Free run, timer started | `timer=3(ON) flags=0x00`, `no change`, `[FREE RUN - no structured step]` |
| 3 | Load a structured workout with a speed target, start it | `tgt=0(SPEED)`, `-> ACT_SPEED <midpoint> km/h` |
| 4 | Hold that step for >30 s | a `KEEP -> re-assert` line |
| 5 | Pause | `timer=2(PAUSED)`, `-> ACT_STOP` |
| 6 | Resume | `timer=3(ON)`, `-> ACT_SPEED` again |
| 7 | End the activity | `LINK  central disconnected - probe reset` |

Check #3's midpoint by hand against the workout's target range: `(lo+hi)/2` in mm/s, times 0.0036.

- [ ] **Step 2: Record the results**

Write `docs/superpowers/test-logs/2026-07-30-mock-bridge-bringup.md` with: the watch model and firmware version, the Connect IQ SDK the loaded `.prg` was built with, each row above marked pass/fail, pasted log excerpts for rows 2-6, and any deviation from the expected output.

If a row fails, that is a finding to write down, not a step to retry until it passes. Record it and stop for review.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/test-logs/2026-07-30-mock-bridge-bringup.md
git commit -m "docs: mock bridge hardware bring-up results"
```

---

## Notes for the implementer

- **Task 1 needs the watch.** Tasks 2 and 3 are pure software and can proceed if the watch is unavailable — but do not build Task 4 on top of an unverified Task 1 gate.
- **`bless` API facts**, verified against the installed version rather than assumed: `is_connected()` and `is_advertising()` are coroutines (`await` them); `start()` takes `prioritize_local_name`; a characteristic object has `.uuid`; `write_request_func` is one global callback for all characteristics, so dispatch on the UUID yourself.
- **macOS cannot scan for its own advertisement.** If you need to inspect what the Mac is broadcasting, use a phone (nRF Connect / LightBlue) or another machine — a `bleak` scan on the same Mac will not see it.
- **Terminal needs Bluetooth permission** (System Settings → Privacy & Security → Bluetooth) or `server.start()` hangs with no error.
