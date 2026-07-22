#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bless"]
# ///
"""Mock FTMS treadmill peripheral for testing the nRF52840 bridge central link.

Advertises the Fitness Machine Service (0x1826) with:
  - Treadmill Data (0x2ACD): notifies simulated run (speed ramps to 12 km/h)
  - Fitness Machine Control Point (0x2AD9): accepts Set Target Speed / Incline writes

Run on macOS (uses CoreBluetooth via bless). The bridge connects as central,
subscribes to notifications, parses the data, and writes speed/incline commands
to the CP which update the simulation.

Port of the original mock_treadmill.py from the ESP32 bridge test harness.
"""

import asyncio
import struct
import time
import threading

from bless import (
    BlessServer,
    GATTCharacteristicProperties as Props,
    GATTAttributePermissions as Perm,
)

FTMS_SVC = "00001826-0000-1000-8000-00805f9b34fb"
TREADMILL_DATA = "00002acd-0000-1000-8000-00805f9b34fb"
FTMS_CP = "00002ad9-0000-1000-8000-00805f9b34fb"

# Treadmill Data flags: bit2 total distance, bit3 inclination, bit10 elapsed.
FLAGS = (1 << 2) | (1 << 3) | (1 << 10)

# FTMS CP opcodes
OP_SET_SPEED = 0x02   # param: uint16 0.01 km/h
OP_SET_INCLINE = 0x03   # param: int16  0.1 %
OP_STOP = 0x08


class Sim:
    """Shared simulation state (written by CP handler, read by notify loop)."""

    lock = threading.Lock()
    speed_kmh = 0.0        # auto-ramp until overridden
    incline_pct = 1.5
    override_speed = False  # True once CP write arrives


sim = Sim()


def frame(speed_kmh: float, dist_m: float, incline_pct: float,
          elapsed_s: int) -> bytes:
    """Build a Fitness Machine Treadmill Data notification (0x2ACD)."""
    b = struct.pack("<H", FLAGS)                          # flags (u16)
    b += struct.pack("<H", int(round(speed_kmh * 100)))   # instantaneous speed (0.01 km/h)
    b += struct.pack("<I", int(round(dist_m)))[:3]        # total distance (u24, m)
    b += struct.pack("<h", int(round(incline_pct * 10)))  # inclination (0.1 %)
    b += struct.pack("<h", 0)                             # ramp angle (unused)
    b += struct.pack("<H", min(elapsed_s, 0xFFFF))        # elapsed time (s)
    return b


def on_cp_write(characteristic, value: bytearray) -> None:
    """Handle Control Point writes from the bridge."""
    if not value:
        return
    op = value[0]
    if op == OP_SET_SPEED and len(value) >= 3:
        kmh = struct.unpack_from("<H", value, 1)[0] / 100.0
        with sim.lock:
            sim.speed_kmh = kmh
            sim.override_speed = True
        print(f"  CP -> set speed {kmh:.2f} km/h")
    elif op == OP_SET_INCLINE and len(value) >= 3:
        pct = struct.unpack_from("<h", value, 1)[0] / 10.0
        with sim.lock:
            sim.incline_pct = pct
        print(f"  CP -> set incline {pct:.1f}%")
    elif op == OP_STOP:
        with sim.lock:
            sim.speed_kmh = 0.0
            sim.override_speed = True
        print("  CP -> stop")


async def main():
    server = BlessServer(name="MockTreadmill")
    server.read_request_func = None
    server.write_request_func = on_cp_write

    await server.add_new_service(FTMS_SVC)
    await server.add_new_characteristic(
        FTMS_SVC, TREADMILL_DATA,
        Props.read | Props.notify, None, Perm.readable,
    )
    await server.add_new_characteristic(
        FTMS_SVC, FTMS_CP,
        Props.write | Props.indicate, None,
        Perm.readable | Perm.writeable,
    )

    await server.start()
    print("MockTreadmill advertising -- FTMS 0x1826 / TD 0x2ACD / CP 0x2AD9")
    print("Speed ramps to 12 km/h; use SPEED command from bridge to override.")

    t0 = time.time()
    dist = 0.0
    last = t0

    try:
        while True:
            await asyncio.sleep(0.5)
            now = time.time()
            elapsed = int(now - t0)
            dt = now - last
            last = now

            with sim.lock:
                if not sim.override_speed:
                    sim.speed_kmh = min(12.0, elapsed * 0.5)
                speed = sim.speed_kmh
                incline = sim.incline_pct

            dist += speed / 3.6 * dt
            val = bytearray(frame(speed, dist, incline, elapsed))
            server.get_characteristic(TREADMILL_DATA).value = val
            server.update_value(FTMS_SVC, TREADMILL_DATA)
            print(
                f"  tx speed={speed:.1f}km/h dist={dist:.1f}m "
                f"incl={incline:.1f}% el={elapsed}s"
            )
    except KeyboardInterrupt:
        pass
    finally:
        await server.stop()


asyncio.run(main())
