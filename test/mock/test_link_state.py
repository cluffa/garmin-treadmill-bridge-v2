#!/usr/bin/env python3
"""Unit tests for the pure link-state decision used by mock_bridge.py.

No bless import here — that's the point. mock_bridge.py has `from bless
import ...` at module scope and can't be imported without bless installed;
link_state.decide() is BLE/ctypes/I-O free specifically so this file can run
without it.
"""
import sys

import link_state as ls


def main():
    # ---- never linked, no write yet: only the WAIT heartbeat can fire ----
    assert ls.decide(now=0.0, last_write=None, linked=False, last_log=0.0) is None
    assert ls.decide(now=ls.WAIT_LOG_S - 0.01, last_write=None, linked=False,
                      last_log=0.0) is None
    assert ls.decide(now=ls.WAIT_LOG_S, last_write=None, linked=False,
                      last_log=0.0) == ls.WAIT

    # ---- first frame ever: LINK fires regardless of elapsed time --------
    assert ls.decide(now=5.0, last_write=5.0, linked=False, last_log=0.0) == ls.LINK
    # Even if a WAIT heartbeat would also be due this tick, LINK takes
    # priority — mock_bridge.py's `continue` after LINK means WAIT/IDLE never
    # gets a chance to fire on the same tick anyway, but decide() itself must
    # pick LINK first since it is the more specific, rarer transition.
    assert ls.decide(now=ls.WAIT_LOG_S, last_write=ls.WAIT_LOG_S, linked=False,
                      last_log=0.0) == ls.LINK

    # ---- steady traffic while linked: no heartbeat until IDLE_LOG_S ------
    assert ls.decide(now=100.0, last_write=100.0, linked=True,
                      last_log=100.0) is None
    assert ls.decide(now=100.0 + ls.IDLE_LOG_S - 0.01, last_write=100.0,
                      linked=True, last_log=100.0) is None
    assert ls.decide(now=100.0 + ls.IDLE_LOG_S, last_write=100.0, linked=True,
                      last_log=100.0) == ls.IDLE

    # A write inside the idle window resets the clock in mock_bridge.py (it
    # sets _last_write = _last_log = now on every write), which shows up here
    # as last_write == last_log == the new "now": no heartbeat is due yet.
    assert ls.decide(now=205.0, last_write=205.0, linked=True,
                      last_log=205.0) is None

    # ---- the stale transition: silence >= STALE_S while linked -----------
    # Isolate the STALE boundary from the (much shorter) IDLE cadence by
    # keeping last_log fresh at each check, as if an idle heartbeat had just
    # printed — otherwise IDLE_LOG_S (10 s) would fire long before STALE_S
    # (120 s) and mask the boundary being tested here.
    just_before = 100.0 + ls.STALE_S - 0.01
    assert ls.decide(now=just_before, last_write=100.0, linked=True,
                      last_log=just_before) is None
    at_stale = 100.0 + ls.STALE_S
    assert ls.decide(now=at_stale, last_write=100.0, linked=True,
                      last_log=at_stale) == ls.STALE
    # STALE takes priority over a simultaneously-due IDLE heartbeat — with a
    # stale last_log too (so IDLE would independently also be due), decide()
    # must still report STALE, not one more "idle" line first.
    assert ls.STALE_S > ls.IDLE_LOG_S
    assert ls.decide(now=at_stale, last_write=100.0, linked=True,
                      last_log=100.0) == ls.STALE

    # A link that was never up cannot go stale, even with a very old
    # last_write and linked=False — LINK must fire instead (or nothing, if
    # LINK already fired and last_write is None again).
    assert ls.decide(now=1000.0, last_write=0.0, linked=False,
                      last_log=1000.0) == ls.LINK

    # ---- silence, then a new frame arrives while still (barely) linked ---
    # mock_bridge.py's on_write() sets both _last_write and _last_log to
    # "now" on every write, so a fresh frame after a long quiet spell reads
    # as last_write == last_log == now here — no heartbeat, no stale, no
    # re-link (linked was already True).
    assert ls.decide(now=500.0, last_write=500.0, linked=True,
                      last_log=500.0) is None

    # ---- regression: stale, then continued silence must NOT oscillate ----
    # This is the bug that actually shipped: clearing `linked` but leaving
    # the old `last_write` in place made the very next tick's
    # `last_write is not None and not linked` branch true again, re-linking
    # and calling probe_reset() every ~2 s forever. mock_bridge.py's fix is
    # to clear _last_write to None in the same tick it clears `linked`, so
    # exercise decide() exactly the way the loop now calls it: once with the
    # pre-stale write time to get STALE, then again with last_write=None
    # (mirroring _last_write = None right after) for several more ticks of
    # continued silence, and confirm none of them come back LINK or STALE.
    stale_now = 100.0 + ls.STALE_S
    assert ls.decide(now=stale_now, last_write=100.0, linked=True,
                      last_log=100.0) == ls.STALE
    linked = False          # what the loop sets on STALE
    last_write = None       # what the loop clears on STALE
    last_log = stale_now    # what the loop sets on STALE
    for tick in range(1, 6):
        now = stale_now + tick
        result = ls.decide(now, last_write, linked, last_log)
        assert result != ls.LINK, f"tick {tick}: spuriously re-linked"
        assert result != ls.STALE, f"tick {tick}: spuriously went stale again"
        # No write ever arrives in this loop, so the only thing that can
        # legitimately fire is the (never-linked) WAIT heartbeat, once
        # WAIT_LOG_S has elapsed since last_log — never IDLE or LINK/STALE.
        if result is not None:
            assert result == ls.WAIT, f"tick {tick}: unexpected {result}"

    print("test_link_state: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
