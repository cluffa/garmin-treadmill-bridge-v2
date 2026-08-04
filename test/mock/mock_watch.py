#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Mock Garmin ctrl app / data field: drive the bridge's A6ED control service.

Connects to the nRF52840 bridge, subscribes to the response characteristic,
decodes the compact 'D'/'E'/'S' frames, and sends workout telemetry frames
to the telemetry characteristic.

Two command families:
  * ctrl grammar (uppercase, to A6ED0002): STATUS, LIST, SCAN, CONNECT <n>,
    SPEED <kmh>, STOP — the picker-app path.
  * workout telemetry (to A6ED0004, decoded by workout_ctrl.c) — the
    data-field path. Commands: TARGET, RANGE, REST, FREERUN, PAUSE,
    RESUME, END.

Options:
  --target <km/h>   Target speed for the workout frame (default 8.0).

Service UUIDs (matching firmware/ble_ctrl_svc.c and the Garmin CIQ apps):
  Service:  A6ED0001-D344-460A-8075-B9E8EC90D71B
  Char 0002: A6ED0002-D344-460A-8075-B9E8EC90D71B  write (ctrl grammar)
  Char 0003: A6ED0003-D344-460A-8075-B9E8EC90D71B  notify (D/E/S frames)
  Char 0004: A6ED0004-D344-460A-8075-B9E8EC90D71B  write (workout telemetry)

This mock discovers the bridge by filtering on the 128-bit service UUID, exactly
as the CIQ apps do — so this base must match the firmware or the mock silently
finds nothing.

15-byte workout telemetry frame layout (little-endian, matches
core/workout_ctrl.h):
  [0]     version           (1)
  [1]     timerState        (1=stopped, 2=paused, 3=on)
  [2]     flags             (bit0 = step present)
  [3]     intensity         (0=active, 1=rest, 0xFF=not set)
  [4]     targetType        (0=speed, 2=open, 0xFF=not set)
  [5..6]  targetLow         uint16, mm/s
  [7..8]  targetHigh        uint16, mm/s
  [9]     durationType      (informational)
  [10..13] durationValue    uint32 (informational)
  [14]    repetitionNumber  (0 if not an interval)
"""
import argparse
import asyncio
import sys
from bleak import BleakScanner, BleakClient

SVC_UUID = "a6ed0001-d344-460a-8075-b9e8ec90d71b"
CTRL_CHR = "a6ed0002-d344-460a-8075-b9e8ec90d71b"
RSP_CHR  = "a6ed0003-d344-460a-8075-b9e8ec90d71b"
WKT_CHR  = "a6ed0004-d344-460a-8075-b9e8ec90d71b"

FLAG_CONNECTED = 0x01
FLAG_SAVED     = 0x02

# Activity enum values mirrored from the watch (see workout_ctrl.h).
TIMER_STOPPED, TIMER_PAUSED, TIMER_ON = 1, 2, 3
TGT_SPEED, TGT_OPEN = 0, 2


def wkt_frame(timer, has_step, target=0xFF, low_mmps=0, high_mmps=0,
              intensity=0xFF, dur_type=0xFF, dur_val=0, rep=0) -> bytes:
    """Pack a 15-byte workout-telemetry frame.

    Wire format (little-endian, 15 bytes):
      [0]     version
      [1]     timerState
      [2]     flags (bit0 = step present)
      [3]     intensity
      [4]     targetType
      [5..6]  targetLow  uint16, mm/s
      [7..8]  targetHigh uint16, mm/s
      [9]     durationType
      [10..13] durationValue uint32
      [14]    repetitionNumber
    """
    f = bytearray(15)
    f[0] = 1                       # version
    f[1] = timer & 0xFF
    f[2] = 0x01 if has_step else 0x00
    f[3] = intensity & 0xFF
    f[4] = target & 0xFF
    f[5:7] = int(low_mmps).to_bytes(2, "little")
    f[7:9] = int(high_mmps).to_bytes(2, "little")
    f[9] = dur_type & 0xFF
    f[10:14] = int(dur_val).to_bytes(4, "little")
    f[14] = rep & 0xFF
    return bytes(f)


def kmh_to_mmps(kmh: float) -> int:
    return int(round(kmh / 3.6 * 1000.0))


def build_telemetry(line: str, target_kmh: float):
    """Return a 15-byte frame for a telemetry command, or None if not a
    telemetry command. Returns b'' for parse-error (handled, nothing to send)."""
    parts = line.split()
    cmd = parts[0].upper()
    try:
        if cmd == "TARGET":
            m = kmh_to_mmps(float(parts[1]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, m, m)
        if cmd == "RANGE":
            lo = kmh_to_mmps(float(parts[1]))
            hi = kmh_to_mmps(float(parts[2]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, lo, hi)
        if cmd == "REST":
            return wkt_frame(TIMER_ON, True, TGT_OPEN, 0, 0)
        if cmd == "FREERUN":
            return wkt_frame(TIMER_ON, False)
        if cmd == "PAUSE":
            return wkt_frame(TIMER_PAUSED, False)
        if cmd == "RESUME":
            m = kmh_to_mmps(float(parts[1]))
            return wkt_frame(TIMER_ON, True, TGT_SPEED, m, m)
        if cmd == "END":
            return wkt_frame(TIMER_STOPPED, False)
        if cmd == "T":
            # Shorthand: send a target-speed frame at the default --target speed.
            m = kmh_to_mmps(target_kmh)
            return wkt_frame(TIMER_ON, True, TGT_SPEED, m, m)
    except (IndexError, ValueError):
        print("  !! bad args for", cmd)
        return b""
    return None


def decode(frame: bytes) -> str:
    """Decode a compact 'D'/'E'/'S' notification frame."""
    if not frame:
        return "<empty>"
    tag = chr(frame[0])
    if tag == "D" and len(frame) >= 5:
        idx = frame[1]
        rssi = int.from_bytes(frame[2:3], "little", signed=True)
        proto = "iFit" if frame[3] else "FTMS"
        flags = frame[4]
        name = frame[5:].split(b"\0")[0].decode(errors="replace")
        marks = ("*" if flags & FLAG_CONNECTED else "") + \
                ("+" if flags & FLAG_SAVED else "")
        return f"D idx={idx} rssi={rssi} proto={proto} name={name!r} {marks}"
    if tag == "E" and len(frame) >= 2:
        return f"E count={frame[1]}"
    if tag == "S" and len(frame) >= 3:
        name = frame[3:].split(b"\0")[0].decode(errors="replace")
        if not frame[1]:
            # proto/name are undefined on a disconnect: ble_ctrl_svc.c's
            # send_status_frame() has no device to read and sends
            # `dev ? dev->proto : 0`, and MACHINE_PROTO_FTMS is 0 — so rendering
            # it printed "disconnected proto=FTMS" after an iFit link dropped,
            # which reads as the bridge flip-flopping between protocols when it
            # is really one machine failing repeatedly. Don't show the field.
            return "S disconnected"
        proto = "iFit" if frame[2] else "FTMS"
        return f"S connected proto={proto} name={name!r}"
    return f"? {frame.hex()}"


async def main():
    parser = argparse.ArgumentParser(description="Mock Garmin watch for TMILL bridge")
    parser.add_argument("--target", type=float, default=8.0,
                        help="Target speed in km/h (default: 8.0)")
    args = parser.parse_args()
    target_kmh = args.target

    print(f"scanning for control service {SVC_UUID} ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SVC_UUID in (ad.service_uuids or []), timeout=15)
    if not dev:
        print("DEVICE NOT FOUND")
        return
    print(f"found: {dev.address} {dev.name}")
    async with BleakClient(dev) as c:
        print(f"connected: {c.is_connected}")

        def on_rsp(_, data: bytearray):
            print("  <-", decode(bytes(data)))

        await c.start_notify(RSP_CHR, on_rsp)
        print(f"subscribed — default target: {target_kmh} km/h")
        print("  ctrl:    STATUS LIST SCAN CONNECT <n> SPEED <kmh> STOP")
        print("  workout: TARGET <kmh> | RANGE <lo> <hi> | REST | "
              "FREERUN | PAUSE | RESUME <kmh> | END | T")
        print("  (Ctrl-D quits)")

        loop = asyncio.get_event_loop()
        while True:
            line = await loop.run_in_executor(None, sys.stdin.readline)
            if not line:            # EOF
                break
            line = line.strip()
            if not line:
                continue
            frame = build_telemetry(line, target_kmh)
            if frame is not None:
                if frame:           # a real frame (not a parse error)
                    await c.write_gatt_char(WKT_CHR, frame, response=False)
                    print(f"  => wkt {frame.hex()}")
            else:
                await c.write_gatt_char(CTRL_CHR, line.upper().encode(),
                                        response=False)
                print(f"  -> {line.upper()}")
            await asyncio.sleep(0.3)   # let replies print before the prompt


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
