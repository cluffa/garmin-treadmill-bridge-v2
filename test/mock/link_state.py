"""Pure link-state decision for mock_bridge.py's 1 Hz asyncio loop.

mock_bridge.py cannot observe its own BLE connection state (see the LINK log
line's own explanation: bless's `is_connected()` is subscription-based and the
data field never subscribes), so link state is inferred from characteristic
write traffic instead. This module is *only* that inference — no BLE, no
ctypes, no I/O, no globals — so it can be unit-tested without bless installed.
mock_bridge.py itself has `from bless import ...` at module scope and is not
importable without it; this module exists so the one real bug this branch
produced (a 1 Hz oscillation between the two LINK lines, `probe_reset()` firing
every ~2 s) has automated coverage instead of only a human watching the log.

The comments on the constants below carry the actual reasoning; see
mock_bridge.py's `_last_write` / `linked` / `STALE_S` for how the asyncio loop
consumes `decide()`'s result.
"""

IDLE_LOG_S = 10.0       # heartbeat cadence while frames are flowing
WAIT_LOG_S = 30.0       # quieter heartbeat before the watch ever appears
# A steady free run legitimately goes minutes without a write (the field sends on
# change), and the gate run showed a 35 s gap while plainly connected. This window
# is deliberately far larger than that — it exists to clear the latched command
# between sessions, not to track the link precisely.
STALE_S = 120.0

# decide()'s return values.
LINK = "link"     # first write ever seen -> declare the link up
STALE = "stale"   # linked, but silent for >= STALE_S -> declare it gone
IDLE = "idle"     # linked, quiet, time for the idle heartbeat
WAIT = "wait"     # never linked, quiet, time for the wait heartbeat


def decide(now, last_write, linked, last_log):
    """Decide what mock_bridge's link-state loop should do on this tick.

    Parameters mirror the loop's local state exactly:
      now        -- time.monotonic() for this tick.
      last_write -- monotonic time of the last characteristic write ever
                    seen, or None if none has been seen yet.
      linked     -- the loop's current `linked` flag.
      last_log   -- monotonic time of the last line the loop printed.

    Returns one of LINK, STALE, IDLE, WAIT, or None ("do nothing this tick").
    The caller (mock_bridge.py's asyncio loop) is responsible for acting on
    the result: logging, flipping `linked`, clearing `last_write`, calling
    probe_reset(), and updating `last_log`. This function only decides —
    it does not mutate anything, which is what makes it safe to call every
    tick and to unit-test in isolation.
    """
    if last_write is not None and not linked:
        return LINK

    # STALE requires linked (a link that was never up cannot go stale) and a
    # real last_write (defensive: STALE otherwise divides "now - None").
    # mock_bridge.py always clears _last_write in the same tick it clears
    # `linked`, so in practice linked implies last_write is not None; this
    # guard just keeps decide() safe to call with any combination of inputs.
    if linked and last_write is not None and now - last_write >= STALE_S:
        return STALE

    if now - last_log >= (IDLE_LOG_S if linked else WAIT_LOG_S):
        return IDLE if linked else WAIT

    return None
