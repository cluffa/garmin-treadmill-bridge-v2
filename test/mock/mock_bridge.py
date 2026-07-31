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
