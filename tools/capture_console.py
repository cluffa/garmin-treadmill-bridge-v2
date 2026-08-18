#!/usr/bin/env python3
"""Timestamped capture of the bridge USB-CDC console.

Every line is prefixed with wall-clock ISO time so the log can be aligned
against a recorded .FIT afterwards.  STATUS is polled periodically because
`scale` (difficulty scaling) and the p70 counters are only observable by
asking -- nothing logs them as events.

Usage: python3 tools/capture_console.py <tty> <outfile> [status_period_s]
"""
import os, select, sys, time
from datetime import datetime

port = sys.argv[1]
outpath = sys.argv[2]
status_period = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
buf = b""
next_status = 0.0

with open(outpath, "a", buffering=1) as out:
    def emit(text):
        out.write("%s %s\n" % (datetime.now().astimezone().isoformat(timespec="milliseconds"), text))

    emit("# capture start port=%s" % port)
    try:
        while True:
            now = time.time()
            if now >= next_status:
                os.write(fd, b"STATUS\n")
                next_status = now + status_period
            r, _, _ = select.select([fd], [], [], 0.25)
            if r:
                buf += os.read(fd, 4096)
                *lines, buf = buf.split(b"\n")
                for ln in lines:
                    emit(ln.decode("utf-8", "replace").rstrip("\r"))
    except KeyboardInterrupt:
        pass
    finally:
        emit("# capture end")
        os.close(fd)
