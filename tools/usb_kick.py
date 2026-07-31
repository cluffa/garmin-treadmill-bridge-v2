#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyusb", "libusb-package"]
# ///
"""Kick the running app's USB device into configuration 1 so macOS binds CDC-ACM.

Why this exists
---------------
After a boot, macOS enumerates the app ("Garmin Treadmill Bridge", 1915:521F),
reads the device and configuration descriptors, and then **never sets the
configuration**.  The device is left with zero `IOUSBHostInterface` children, so
`AppleUSBACMData` never attaches and no `/dev/cu.usbmodem<serial>1` node exists.
No console, and no way to send the `DFU` ctrl command over USB.

Measured, not guessed (2026-07-31):
  * EP0 is healthy — GET_DESCRIPTOR device *and* the 75-byte multi-packet
    config descriptor both transfer fine.  This is **not** the "marginal USB-C
    cable" that docs/flashing.md used to blame; a bad cable cannot serve a
    multi-packet EP0 IN and then selectively stall one request.
  * Immediately after boot the device **stalls** GET_CONFIGURATION and
    SET_CONFIGURATION.  `app_usbd` runs with
    `APP_USBD_CONFIG_EVENT_QUEUE_ENABLE 1`, so those are completed from
    `app_usbd_event_queue_process()` in the main loop, while descriptor reads
    are answered lower down.  macOS asks once, gets a stall, and gives up
    forever.
  * A USB bus reset followed by a manual SET_CONFIGURATION(1) succeeds, and
    macOS then instantiates the interfaces and the tty appears.
  * The bootloader never needs this — it boots straight into a tight loop and
    wins the same race.

So the workaround is: bus reset, then SET_CONFIGURATION(1).  Re-run after every
app boot (including after a DFU update).

Usage
-----
    make usb-kick          # or: tools/usb_kick.py
"""
import glob
import os
import sys
import time

import libusb_package
import usb.core
import usb.util

VID, PID = 0x1915, 0x521F
APP_PRODUCT = "Garmin Treadmill Bridge"


def find(backend):
    return usb.core.find(idVendor=VID, idProduct=PID, backend=backend)


def product_of(dev):
    try:
        return usb.util.get_string(dev, dev.iProduct)
    except Exception:
        return "?"


def get_configuration(dev):
    """GET_CONFIGURATION over EP0. Returns None if the device stalls it."""
    try:
        return dev.ctrl_transfer(0x80, 0x08, 0, 0, 1, timeout=2000)[0]
    except usb.core.USBError:
        return None


def main():
    backend = libusb_package.get_libusb1_backend()
    serial = None

    # Called right after a DFU update the device is still re-enumerating, so
    # every handle here can go stale mid-sequence ("No such device"). Retry the
    # whole find → reset → configure sequence rather than failing the run.
    for attempt in range(12):
        try:
            dev = find(backend)
            if dev is None:
                time.sleep(1)
                continue

            product = product_of(dev)
            try:
                serial = usb.util.get_string(dev, dev.iSerialNumber)
            except Exception:
                serial = None
            if product != APP_PRODUCT:
                # Still in the bootloader (which configures itself correctly),
                # or mid-transition. Wait for the app rather than poking it.
                print(f"waiting for the app (currently {product!r})...")
                time.sleep(2)
                continue
            if attempt == 0:
                print(f"found {product!r} serial={serial}")

            if get_configuration(dev) == 1:
                print("already configured")
                break

            # The bus reset is what makes the device accept SET_CONFIGURATION;
            # without it the request stalls just as it did for macOS.
            dev.reset()
            print("bus reset issued")
            time.sleep(2)

            dev = find(backend)
            if dev is None:
                continue

            dev.ctrl_transfer(0x00, 0x09, 1, 0, None, timeout=2000)
            print("SET_CONFIGURATION(1) accepted")
            break
        except usb.core.USBError as e:
            # Stalled or vanished — both are transient here.
            print(f"retrying ({e})")
            time.sleep(2)
    else:
        print("error: gave up trying to configure the device")
        return 1

    # macOS names the node after the chip serial: /dev/cu.usbmodem<SERIAL>1.
    # Match on that rather than on whatever else is plugged in — with the Pico
    # probe attached there are two usbmodem nodes and only one is the app.
    node = f"/dev/cu.usbmodem{serial}1" if serial else None
    for _ in range(10):
        time.sleep(1)
        if node and os.path.exists(node):
            print(f"console: {node}")
            return 0
        if node is None:
            found = glob.glob("/dev/cu.usbmodem*")
            if found:
                print("console (unverified): " + " ".join(found))
                return 0
    print("warning: configuration set but no tty appeared — check `ls /dev/cu.usbmodem*`")
    return 1


if __name__ == "__main__":
    sys.exit(main())
