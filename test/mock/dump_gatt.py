#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Dump the bridge's GATT database exactly as this host sees it.

Diagnostic for "characteristic not found" after a UUID change. macOS caches a
peripheral's GATT database keyed by its address, and the cache is NOT
invalidated when the firmware changes: the *advertisement* is read live off the
radio (so a new service UUID shows up in scan results immediately), while the
service/characteristic list served on connect can be stale. The symptom is a
successful connect followed by BleakCharacteristicNotFoundError.

This tells the two apart:
  - old UUIDs listed  -> stale host cache; the firmware is fine
  - new UUIDs listed  -> the host is current; look at the firmware instead
  - nothing listed    -> discovery itself failed

Finds the bridge by NAME, not by service UUID, so it works regardless of which
base the host currently believes in.
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "TMILL-CTRL"
NEW_BASE = "d344-460a-8075-b9e8ec90d71b"
OLD_BASE = "2e7a-4e1d-9e3b-000000000000"


async def main():
    print(f"scanning for {DEVICE_NAME} by name ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (ad.local_name or d.name or "") == DEVICE_NAME, timeout=15)
    if dev is None:
        print("DEVICE NOT FOUND — is the board advertising? "
              "A connected peripheral link stops advertising.")
        return 1
    print(f"found: {dev.address} {dev.name}")

    async with BleakClient(dev) as c:
        print(f"connected: {c.is_connected}\n")
        seen = []
        for svc in c.services:
            print(f"service {svc.uuid}")
            for ch in svc.characteristics:
                props = ",".join(ch.properties)
                print(f"    char {ch.uuid}  [{props}]")
                seen.append(ch.uuid.lower())

        new = [u for u in seen if NEW_BASE in u]
        old = [u for u in seen if OLD_BASE in u]
        print("\n---- verdict ----")
        if old and not new:
            print("STALE HOST CACHE. The database served here still uses the OLD")
            print("base. The firmware is not at fault.")
            print("Fix: toggle Bluetooth off/on on this Mac (System Settings >")
            print("Bluetooth), or `sudo pkill bluetoothd`, then retry.")
        elif new:
            print(f"Host is CURRENT — {len(new)} characteristic(s) on the new base.")
            print("If a mock still fails, the bug is elsewhere.")
        else:
            print("Neither base found. Discovery returned no A6ED characteristics")
            print("at all — check that the service actually registered on-device.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        pass
