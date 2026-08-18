#!/usr/bin/env python3
"""Log another ANT+ sensor through the bridge, to compare against our own footpod.

The bridge transmits a synthesized ANT+ SDM footpod (firmware/ant_sdm.c). This
drives the second ANT channel added in firmware/ant_log.c, which locks onto some
OTHER sensor in the room -- a real footpod, or a treadmill's own ANT+ broadcast
-- and dumps every page it receives to the USB console. Both sides are then
timestamped by the same board, off the same app_timer, so the two traces line up
without any clock reconciliation.

Usage:
    tools/ant_logger.py <tty> [--type N] [--dev N] [--period N] [--freq N]
                             [--out FILE] [--status-period S] [--quiet]

    tools/ant_logger.py /dev/cu.usbmodemXXXX            # wildcard: lock onto
                                                        # whatever is nearest
    tools/ant_logger.py /dev/cu.usbmodemXXXX --type 124 # pin to an SDM footpod

With no --type it searches with a wildcard id, reports what it found, and then
RE-OPENS the channel at that profile's message period (they differ: SDM 8134,
HRM 8070, most others 8192). A slave whose period disagrees with the master it
locked onto receives the first message and then drops most of the rest, which
looks exactly like a sensor with terrible reception -- so the retune is not
cosmetic.

Writes newline-delimited JSON to logs/antlog-<timestamp>.jsonl, one record per
received ANT message plus the console's own events, and prints a one-line
summary per second. Ctrl-C to stop; the channel is closed on the way out so it
stops costing the footpod broadcast radio time.
"""

import argparse
import json
import os
import select
import signal
import sys
import time
from datetime import datetime

# ANT+ profile message periods, 32768 Hz counts. Same table as
# firmware/ant_log.c default_period() -- kept here too so the host can tell that
# a lock happened at the wrong period and fix it.
PROFILE_PERIOD = {124: 8134, 120: 8070}
DEFAULT_PERIOD = 8192

PROFILE_NAME = {
    124: "SDM (stride speed/distance)",
    120: "HRM (heart rate)",
    17:  "FE-C (fitness equipment)",
    11:  "bike power",
    121: "bike speed+cadence",
}


def decode_sdm(p):
    """Decode an ANT+ SDM page into a dict, or None for a page we don't read.

    Speed lives in byte 4's low nibble (integer m/s) and byte 5 (1/256 m/s).
    Nordic's SDK marks both 'reserved' in its page-1 struct; the ANT+ SDM
    profile puts instantaneous speed there and that is what real sensors --
    and core/ant_sdm_encode.c -- transmit.
    """
    page = p[0] & 0x7F           # bit 7 is the page-toggle, not the number
    if page == 1:
        return {
            "page": 1,
            "time_s": p[2] + p[1] / 256.0,
            "dist_m": p[3] + (p[4] >> 4) / 16.0,
            "speed_mps": (p[4] & 0x0F) + p[5] / 256.0,
            "strides": p[6],
            "latency_s": p[7] / 32.0,
        }
    if page == 2:
        cad = None if p[3] == 0xFF else p[3] + (p[4] >> 4) / 16.0
        return {
            "page": 2,
            "cadence_spm": cad,
            "speed_mps": (p[4] & 0x0F) + p[5] / 256.0,
            "use_state": p[7] & 0x03,
            "battery": (p[7] >> 4) & 0x03,
        }
    if page == 22:
        return {
            "page": 22,
            "cap_time": bool(p[1] & 0x01),
            "cap_distance": bool(p[1] & 0x02),
            "cap_speed": bool(p[1] & 0x04),
            "cap_latency": bool(p[1] & 0x08),
            "cap_cadence": bool(p[1] & 0x10),
            "cap_calories": bool(p[1] & 0x20),
        }
    if page == 80:
        return {"page": 80, "hw_rev": p[3],
                "manufacturer": p[4] | (p[5] << 8), "model": p[6] | (p[7] << 8)}
    if page == 81:
        return {"page": 81, "sw_rev": p[3],
                "serial": p[4] | (p[5] << 8) | (p[6] << 16) | (p[7] << 24)}
    return {"page": page}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tty")
    ap.add_argument("--type", type=int, default=0, help="ANT device type (0 = any)")
    ap.add_argument("--dev", type=int, default=0, help="ANT device number (0 = any)")
    ap.add_argument("--period", type=int, default=0,
                    help="channel period, 32768 Hz counts (0 = from device type)")
    ap.add_argument("--freq", type=int, default=0, help="ANT RF frequency (0 = 57)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--status-period", type=float, default=15.0,
                    help="seconds between ANTLOG/STATUS polls (0 = never)")
    ap.add_argument("--no-retune", action="store_true",
                    help="do not re-open at the locked profile's period")
    ap.add_argument("--quiet", "-q", action="store_true")
    args = ap.parse_args()

    # Run the cleanup on SIGTERM too, not just Ctrl-C. Killed without it, the
    # sniffer channel stays open on the device: it keeps costing the footpod
    # broadcast radio time, and the next run's first "locked" line comes from
    # the ORPHANED session with its own message counter -- which is exactly the
    # stale-state confusion the initial ANTLOG STOP below exists to avoid.
    # sys.exit raises SystemExit, so the finally block still runs.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = args.out or os.path.join(
        repo, "logs", "antlog-%s.jsonl" % datetime.now().strftime("%Y%m%dT%H%M%S"))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    fd = os.open(args.tty, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)

    def send(cmd):
        os.write(fd, (cmd + "\n").encode())

    def start_cmd(devtype, dev, period, freq):
        return "ANTLOG START %d %d %d %d" % (devtype, dev, period, freq)

    out = open(out_path, "a", buffering=1)

    def rec(kind, **kw):
        kw["t"] = datetime.now().astimezone().isoformat(timespec="milliseconds")
        kw["kind"] = kind
        out.write(json.dumps(kw) + "\n")

    rec("start", tty=args.tty, type=args.type, dev=args.dev,
        period=args.period, freq=args.freq)
    print("logging to %s" % out_path)

    # Close any channel a previous run left open before starting our own. Not
    # just tidiness: its in-flight lines would otherwise arrive labelled with
    # the old session's message counter and its lock, and the first "locked"
    # line is what the retune below reacts to -- reacting to a stale one
    # retunes to the wrong thing at the wrong moment.
    send("ANTLOG STOP")
    time.sleep(0.6)
    try:
        while select.select([fd], [], [], 0)[0]:
            if not os.read(fd, 4096):
                break
    except BlockingIOError:
        pass

    send(start_cmd(args.type, args.dev, args.period, args.freq))

    buf = b""
    retuned = args.no_retune
    next_status = time.time() + args.status_period
    last_summary = 0.0
    last_seq = None
    gaps = 0
    n_msgs = 0
    latest = None
    locked_desc = None

    try:
        while True:
            now = time.time()
            if args.status_period and now >= next_status:
                send("ANTLOG")
                send("STATUS")
                next_status = now + args.status_period

            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                buf += os.read(fd, 4096)
                *lines, buf = buf.split(b"\n")
                for raw in lines:
                    line = raw.decode("utf-8", "replace").rstrip("\r")
                    if not line:
                        continue

                    if line.startswith("AL "):
                        f = line.split()
                        if len(f) != 7:
                            rec("malformed", line=line)
                            continue
                        seq, ms, dev, dtype, trans = (int(x) for x in f[1:6])
                        payload = bytes.fromhex(f[6])
                        # A gap in the device-side sequence number is a radio
                        # dropout; a gap in wall-clock arrival could just be USB
                        # or this process being descheduled. Only the former is
                        # evidence about the sensor.
                        if last_seq is not None:
                            if seq > last_seq + 1:
                                gaps += seq - last_seq - 1
                            elif seq <= last_seq:
                                # The counter went backwards: the firmware
                                # re-opened the channel and zeroed it. That is
                                # a restart, not a dropout -- counting it as
                                # one would make every retune look like the
                                # sensor had just missed a message.
                                rec("seq_restart", seq=seq, prev=last_seq)
                        last_seq = seq
                        n_msgs += 1
                        d = decode_sdm(payload) if dtype in (124, 0) else {}
                        latest = d
                        rec("msg", seq=seq, ms=ms, dev=dev, type=dtype,
                            trans=trans, hex=f[6], **d)
                        continue

                    # Console JSON: antlog state events, ANTLOG/STATUS replies,
                    # and the bridge's own log lines (kept -- they are how the
                    # treadmill link and the difficulty scale get into the same
                    # timeline as the sensor).
                    try:
                        obj = json.loads(line)
                    except ValueError:
                        rec("log", line=line)
                        continue
                    rec("console", **obj)

                    # The periodic status doubles as a watchdog. Every way this
                    # channel can die -- a failed re-open, a close we did not
                    # ask for -- ends with running:false and no further lines,
                    # which is indistinguishable from a sensor that went quiet
                    # unless something checks. Re-issue the start rather than
                    # logging silence.
                    if (obj.get("cmd") == "antlog" and "running" in obj
                            and not obj["running"] and not obj.get("pending")):
                        want_type = obj.get("cfg", {}).get("type", args.type)
                        want_dev = obj.get("cfg", {}).get("dev", args.dev)
                        want_period = obj.get("cfg", {}).get("period", args.period)
                        print("\nchannel not running -- restarting")
                        rec("restart", type=want_type, dev=want_dev,
                            period=want_period)
                        send(start_cmd(want_type, want_dev, want_period,
                                       args.freq))
                        last_seq = None
                        continue

                    if obj.get("event") == "antlog" and obj.get("state") == "locked":
                        dtype = obj.get("type", 0)
                        locked_desc = "dev %s type %s (%s)" % (
                            obj.get("dev"), dtype,
                            PROFILE_NAME.get(dtype, "unknown profile"))
                        print("locked: %s" % locked_desc)
                        want = PROFILE_PERIOD.get(dtype, DEFAULT_PERIOD)
                        if not retuned and args.period == 0:
                            retuned = True
                            # Re-open pinned to what we found, at that
                            # profile's period. Pinning the device number too
                            # stops a re-search wandering onto another sensor
                            # halfway through the comparison.
                            print("retuning to period %d, pinned to dev %s"
                                  % (want, obj.get("dev")))
                            rec("retune", type=dtype, dev=obj.get("dev"),
                                period=want)
                            send(start_cmd(dtype, obj.get("dev", 0), want,
                                           args.freq))
                            last_seq = None

            if not args.quiet and now - last_summary >= 1.0:
                last_summary = now
                if latest and latest.get("page") == 1:
                    print("\r%5d msgs  %d gaps  %s  %5.2f m/s (%4.1f km/h)  "
                          "%7.1f m  t=%6.1f s" % (
                              n_msgs, gaps, locked_desc or "searching",
                              latest["speed_mps"], latest["speed_mps"] * 3.6,
                              latest["dist_m"], latest["time_s"]),
                          end="", flush=True)
                elif not locked_desc:
                    print("\r%5d msgs  searching..." % n_msgs, end="", flush=True)
    except (KeyboardInterrupt, SystemExit):
        print()
    except OSError as e:
        # The console node went away underneath us -- the board re-enumerated,
        # or macOS dropped the CDC configuration (it needs `make usb-kick`
        # after every app boot; see docs/flashing.md 8a). This is not a reason
        # to lose a capture: the JSONL is line-buffered, so everything up to
        # this instant is already on disk, and a traceback at minute 50 of a
        # workout would read as "the tool broke" rather than "the cable did".
        print("\nconsole disconnected (%s)" % e)
        print("the log up to this point is intact; to resume:")
        print("  make usb-kick && %s %s" % (sys.argv[0], args.tty))
    finally:
        try:
            send("ANTLOG")
            time.sleep(0.3)
            r, _, _ = select.select([fd], [], [], 0.3)
            if r:
                tail = os.read(fd, 4096).decode("utf-8", "replace")
                for line in tail.splitlines():
                    if "antlog" in line:
                        print(line)
                        rec("log", line=line)
            send("ANTLOG STOP")
        except OSError:
            pass
        rec("end", msgs=n_msgs, gaps=gaps)
        out.close()
        os.close(fd)
        print("%d messages, %d dropped by the radio -> %s"
              % (n_msgs, gaps, out_path))


if __name__ == "__main__":
    main()
