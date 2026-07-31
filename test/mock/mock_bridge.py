#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# Pinned, not "bless": the advertising behaviour this whole design rests on
# (prioritize_local_name, the `len(name) > 10` drop-the-service-UUID rule —
# see the comment on ADV_NAME below) is a bless 0.3.0 corebluetooth/server.py
# implementation detail, not a documented contract. An unpinned upgrade can
# change or remove it silently; the watch would simply stop finding the mock,
# with nothing here to explain why.
# dependencies = ["bless==0.3.0"]
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
import ctypes
import pathlib
import threading
import time
from datetime import datetime

from bless import (
    BlessServer,
    GATTCharacteristicProperties as Props,
    GATTAttributePermissions as Perm,
)

import link_state
import wkt_decode as wkt

# Must match firmware/ble_ctrl_svc.c. test/check_uuid_contract.py enforces this.
CTRL_SVC = "A6ED0001-D344-460A-8075-B9E8EC90D71B"
CTRL_CHR = "A6ED0002-D344-460A-8075-B9E8EC90D71B"  # write (ctrl grammar)
CTRL_RSP = "A6ED0003-D344-460A-8075-B9E8EC90D71B"  # notify (D/E/S frames)
CTRL_WKT = "A6ED0004-D344-460A-8075-B9E8EC90D71B"  # write (workout telemetry)

# The 8-hex-char prefix (service base + characteristic alias) that identifies
# a write as the workout characteristic. Derived from CTRL_WKT itself, not
# repeated as a second literal, so on_write() cannot silently drift from the
# constant above and log every real workout frame as "(ignored)".
CTRL_WKT_PREFIX = CTRL_WKT.split("-", 1)[0]

# <= 10 chars: bless drops service UUIDs from the advert for longer names when
# prioritize_local_name is true. We pass False anyway, but a short name keeps
# this correct under either setting. "MOCK" distinguishes it from the real
# bridge in a phone scanner.
ADV_NAME = "TMILL-MOCK"

LIB = pathlib.Path(__file__).with_name("libworkout_probe.so")

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

# Written from the CoreBluetooth callback thread, read from the asyncio loop.
# Bare assignments to a float/None are atomic under the GIL, so no lock is needed.
_last_write = None      # monotonic time of the last characteristic write, ever
_last_log = time.monotonic()   # monotonic time of the last line the loop printed

# Cadence/timeout constants and the actual link-state decision live in
# link_state.py — a pure module (no bless/ctypes/I-O) so it is unit-testable
# without bless installed. See test/mock/test_link_state.py.
IDLE_LOG_S = link_state.IDLE_LOG_S
WAIT_LOG_S = link_state.WAIT_LOG_S
STALE_S = link_state.STALE_S


def log(tag: str, msg: str) -> None:
    ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    print(f"{ts}  {tag:<5} {msg}", flush=True)


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
    if not uuid.startswith(CTRL_WKT_PREFIX):
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


async def main() -> None:
    global probe, _last_log, _last_write

    probe = load_probe()
    probe.probe_reset()

    server = BlessServer(name=ADV_NAME)
    # No read_request_func: none of the three characteristics below carry
    # Perm.readable, matching firmware/ble_ctrl_svc.c's GATT table, which
    # declares all three write/notify-only (no `.read` in any char_props).
    # bless does not require a readable permission to create a characteristic
    # (verified empirically), so this is a straight permissions fix, not a
    # workaround. Previously all three were Perm.readable with
    # read_request_func left None: CoreBluetooth honours the ATT permission
    # bit independently of GATT properties, so it dispatched real reads from
    # tools like nRF Connect/LightBlue straight into a callback that raises
    # BlessError("Server: Read Callback is undefined") and never completes.
    server.read_request_func = None
    server.write_request_func = on_write

    await server.add_new_service(CTRL_SVC)
    # write + write_without_response mirrors firmware/ble_ctrl_svc.c:482,532.
    # CtrlBleDelegate uses WRITE_TYPE_DEFAULT (write *with* response), so plain
    # `write` is the one that actually matters here.
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_CHR,
        Props.write | Props.write_without_response, None,
        Perm.writeable,
    )
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_RSP,
        Props.notify, None, Perm(0),
    )
    await server.add_new_characteristic(
        CTRL_SVC, CTRL_WKT,
        Props.write | Props.write_without_response, None,
        Perm.writeable,
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

    linked = False
    try:
        while True:
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

            # Pure decision (no BLE/ctypes/I-O) — see link_state.py and
            # test/mock/test_link_state.py. This loop only acts on the result.
            decision = link_state.decide(now, _last_write, linked, _last_log)

            if decision == link_state.LINK:
                linked = True
                _last_log = now
                log("LINK", "frames arriving - watch attached (inferred from "
                            "traffic; bless is_connected() is subscription-based "
                            "and the data field never subscribes)")
                continue

            if decision == link_state.STALE:
                linked = False
                # Also clear _last_write, not just `linked`: the next tick's
                # decide() call re-links as soon as `_last_write is not None`,
                # so leaving the old timestamp in place makes it immediately
                # true again on the very next tick, and then true forever (the
                # timestamp only gets older) — an infinite 1 Hz oscillation
                # between "frames arriving" and this branch, calling
                # probe_reset() on every other tick instead of once. Clearing
                # it returns to the exact pre-watch "no write ever seen"
                # state, so a real new frame is required before the mock
                # claims a link again. (Regression-tested in
                # test_link_state.py: a stale transition followed by
                # continued silence must not re-link or reset repeatedly.)
                _last_write = None
                # Drop the latched command so a stale keepalive does not survive
                # into the next session.
                with _probe_lock:
                    probe.probe_reset()
                _last_log = now
                log("LINK", f"no frames for {STALE_S:.0f}s - assuming the watch "
                            f"is gone; probe reset")
                continue

            if decision in (link_state.IDLE, link_state.WAIT):
                _last_log = now
                if decision == link_state.IDLE:
                    log("idle", "(no write - field sends on change only)")
                else:
                    log("wait", "no frames yet - start a run activity with the "
                                "data field on a screen")
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        await server.stop()
        log("ADV", "stopped")


if __name__ == "__main__":
    asyncio.run(main())
