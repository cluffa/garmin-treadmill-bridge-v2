#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bless"]
# ///
"""Mock FTMS treadmill that accepts a connection and then fails GATT discovery.

Exists to exercise the failed-attempt backoff (core/connect_backoff.c) on real
hardware. mock_treadmill.py is a treadmill that *works*; this is one that looks
like a treadmill right up until the bridge tries to use it, which is the case
the backoff was written for and the hardest one to reproduce with real gear.

It advertises the Fitness Machine Service (0x1826) so the bridge classifies it
as a treadmill and will connect, but exposes ONLY the Control Point (0x2AD9) —
no Treadmill Data (0x2ACD). The bridge's discovery therefore ends in
disc_finish_chrs() with s_data_handle == 0, logs

    central: notify characteristic not found

and calls gattc_fail(), which disconnects and arms the backoff. Each attempt
should log an escalating cooldown:

    central: attempt 1 never became usable — not auto-retrying for 1000 ms
    central: attempt 2 never became usable — not auto-retrying for 2000 ms
    central: attempt 3 never became usable — not auto-retrying for 4000 ms

A count that keeps reading "attempt 1" is the single-slot bug back again.

NOTE the bridge will only *auto*-pick this device when the saved treadmill is
not advertising: connect_policy_choose() returns the saved device outright
whenever it is in the list (core/connect_policy.c). With the real treadmill
powered on, use a manual `CONNECT <n>` from the console instead — that path
still arms the backoff, it just bypasses the suppression check by design.
"""

import asyncio

from bless import (
    BlessServer,
    GATTCharacteristicProperties as Props,
    GATTAttributePermissions as Perm,
)

FTMS_SVC = "00001826-0000-1000-8000-00805f9b34fb"
FTMS_CP = "00002ad9-0000-1000-8000-00805f9b34fb"

# Deliberately short. bless drops every service UUID from the advertisement when
# the local name is longer than 10 characters, even with prioritize_local_name
# False in some paths — mock_treadmill.py documents the trap. A device with no
# 0x1826 in its advert is not a treadmill as far as the bridge is concerned, and
# this mock would silently never be picked.
NAME = "BadTmill"


def on_cp_write(characteristic, value: bytearray) -> None:
    # Never actually reached: the bridge gives up at discovery, before it has a
    # control-point handle to write to. Present only so the characteristic is
    # writable and the service looks plausible.
    print(f"  CP <- {bytes(value).hex()}  (unexpected: discovery should fail first)")


async def main():
    server = BlessServer(name=NAME)
    server.write_request_func = on_cp_write

    await server.add_new_service(FTMS_SVC)

    # The Control Point only. Omitting Treadmill Data (0x2ACD) is the entire
    # point of this mock — it is what makes discovery fail.
    await server.add_new_characteristic(
        FTMS_SVC, FTMS_CP,
        Props.write | Props.indicate, None,
        Perm.readable | Perm.writeable,
    )

    # See mock_treadmill.py: this is required, not a preference.
    await server.start(prioritize_local_name=False)
    print(f"{NAME} advertising — FTMS 0x1826 with CP 0x2AD9 and NO 0x2ACD")
    print("Expect the bridge to connect, fail discovery, and back off.")
    print("Watch its console for 'notify characteristic not found' then")
    print("'attempt N never became usable' with N climbing. Ctrl-C to stop.")

    try:
        while True:
            await asyncio.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        await server.stop()


asyncio.run(main())
