# Two open investigations: CDC drop counter, and CIQ scan recovery

*Task brief, written 2026-08-03. Both items were left open deliberately because
they were not proven, not because they were finished.*

**Read first:** `CLAUDE.md`, then `docs/HANDOFF.md` (top section + "Known gaps").
HANDOFF is the project's handoff convention and already contains the evidence
trail for both items — including a failed fix attempt you must not repeat.

The two tasks are independent. **Task B is the higher-value one** (it is
user-facing); Task A is a diagnostic-integrity issue. Do them in either order,
but do not start either by proposing a fix.

---

## Task A — `tx_drops` is proven plumbed, not proven accurate

`firmware/usb_cdc_log.c` keeps `s_cdc_tx_drops`, a count of console bytes lost
when the TX FIFO overflows or `app_usbd_cdc_acm_write()` refuses a write. It is
surfaced through a weak `ctrl_console_drops()` (declared `core/ctrl_dispatch.h`,
strong definition in `usb_cdc_log.c`) and appears in the `STATUS` reply:

```
{"cmd":"status","connected":true,"name":"I_TL","tx_drops":0}
```

Verified so far: the field is present, and the map file confirms
`ctrl_console_drops` resolves to `usb_cdc_log.c.o`, so it is the real counter
and not the weak `0` stub. **A non-zero value has never been observed.**

Two specific problems:

1. **I could not force the FIFO (1 KB) to overflow.** macOS buffers the tty even
   when no process is reading, so the device keeps draining and never backs up.
   Without ever seeing a non-zero value, the counting logic is unexercised —
   the increments in `fifo_push()` and `cdc_tx_pump()` could be wrong and
   nothing would show it.
2. **`PORT_OPEN` resets the counter to 0.** Reconnecting the tty to *read*
   `STATUS` zeroes exactly the history you wanted. That makes it useless for the
   most valuable question: "did I lose output before I attached?" This may be a
   design mistake rather than an implementation one — consider whether the
   counter should be cumulative since boot, with the per-session reset either
   dropped or exposed as a second field.

**What I want:** determine whether the counter is correct, and make it possible
to trust. Ideas worth considering (not prescriptive): a temporary build with a
tiny `CDC_TX_FIFO_SIZE` to make overflow trivial to trigger; a debug command
that emits a large burst; stalling the host with the port open; or a host-side
unit test of the FIFO logic if it can be extracted the way
`core/connect_backoff.c` was. If you conclude the reset-on-open is wrong, say so
and change it — but justify it.

Relevant background: this counter matters because the console *did* silently
truncate everything past one endpoint packet until 2026-08-03, and that
truncation masked two separate bugs for a long time (a log line cut mid-word,
and a `LIST` reply cut mid-JSON so device indices were unreadable). A console
that loses output silently is the failure mode this counter exists to prevent.
See commit `d1c5fec`.

---

## Task B — the Connect IQ data field cannot recover a dead BLE scan

**The defect** (present in both `watch/garmin_data_field/source/CtrlBleDelegate.mc`
and `watch/garmin_ctrl_app/source/BridgeBle.mc`):

`mScanning` is *observed* radio state — written only inside the
`onScanStateChange` callback, so it is always one callback behind. It is used as
the guard for *intent* in `startScan()`. In `onScanResults`:

```monkeyc
BluetoothLowEnergy.setScanState(SCAN_STATE_OFF);   // async
try { mDevice = BluetoothLowEnergy.pairDevice(r); }
catch (e) { startScan(); }                          // early-returns: mScanning still true
```

A `pairDevice()` throw calls `startScan()` while `mScanning` is *still* `true`,
so it early-returns; the pending `OFF` callback then lands and sets it false; and
**nothing ever restarts the scan**. The field sits at `--` (not connected, not
scanning) for the rest of that activity and across later ones, because a stale
pairing keeps `pairDevice()` throwing. A run that dies without reaching
`onStop()` → `shutdown()` → `unpairDevice()` is enough to arm it.

**A fix was attempted on 2026-08-03 and REVERTED. Do not re-apply it as
written.** It split the state into `mScanning` + `mWantScan` and reconciled them
by calling `setScanState(SCANNING)` *from inside the `onScanStateChange`
callback*, plus a 1 Hz `startScan()` backstop from `compute()`. On hardware the
field went from `CONN` with a live target pace to `--` with no pace at all, and
the bridge's testboard showed `W:-`. Connect IQ does not tolerate re-entering the
BLE stack from within a BLE callback. See `docs/HANDOFF.md` "Known gaps" for the
full write-up and commit `d0647a5`.

**Constraints any retry must satisfy:**

- Re-arm the scan from a **deferred** context — e.g. set a flag in the callback
  and consume it on the next `compute()` — never call into `BluetoothLowEnergy`
  from inside a BLE callback.
- Do not call `setScanState()` more than once per state transition. No polling
  or hammering.
- Fail safe: if the recovery path does nothing, behaviour must degrade to
  today's (a scan that stays dead), never to something worse.

**Critical testing constraint — this cost most of an evening:**

A wedged Connect IQ BLE stack **survives reinstalling the app**. Only a watch
power-cycle clears it. So:

- **A revert that "doesn't fix it" proves nothing** — you may be measuring the
  previous build's wreckage.
- **Reboot the watch between test builds**, every time.
- **Read the build stamp off the field's bottom row before trusting any
  result.** `tools/ciq_stamp.sh` generates `source/BuildInfo.mc`; the field
  renders it as e.g. `CONN 0803-1932`. If it does not match the build you just
  made, the install did not take and every downstream observation is unsound.
  This is the single most useful diagnostic in the project — use it first.

**Also worth fixing or at least knowing:** `isConnected()` is just
`mDevice != null`, and `pairDevice()` sets `mDevice` *before* the link is up. So
the field can display `CONN` while nothing works. `CONN` means "pairing
requested", not "link up". Deciding whether to tighten this is in scope.

---

## Environment and tooling

**Build / test**
```sh
make host-test     # 10 host suites + UUID contract. Must stay green.
make ciq-build     # stamps BuildInfo.mc, THEN runs monkeyc -l 2 (strict)
make sideload      # pushes the .prg over MTP; CONSUMES an existing build
make firmware      # nRF52840 + S340
```

**Flashing the bridge — SWD is dead, USB-DFU is the only route:**
```sh
make usb-kick                       # no tty until a manual SET_CONFIGURATION
printf 'DFU\n' > /dev/cu.usbmodemXXXX   # app reboots into the bootloader
make flash-dfu SERIAL=/dev/cu.usbmodemXXXX
```
`make flash-app` is **broken** — it parks the board in the bootloader. After any
flash, confirm the app actually booted: the console heartbeat `alive N` must be
a small, monotonically increasing number. Garbage or silence means the
bootloader still has control.

**Watch over MTP:** it must be awake and unlocked, and must enumerate as USB
product `20917` (`0x51b5`). Product `3` with no product name means MTP is not
being presented — unplug, wake/unlock, replug. `libmtp` calls *block* rather
than erroring, and killing one wedges `libmtp` until a physical replug, so never
`kill -9` an MTP command. `tools/ciq_sideload.sh` wraps everything in timeouts.

**Useful harnesses:**
- `test/mock/mock_bridge.py` — a macOS BLE peripheral impersonating the bridge,
  so the data field can be debugged with no nRF hardware in the loop. Probably
  the fastest way to iterate on Task B.
- `uv run --script test/mock/mock_watch.py` — connects to the real bridge from
  the Mac, filtering on the same 128-bit UUID the CIQ app uses. Proves in
  seconds whether the bridge is advertising and connectable.
- `test/mock/mock_bad_treadmill.py` — advertises FTMS but fails GATT discovery.
- Watch logs: `GARMIN/APPS/LOGS/CIQ_LOG.YML` over MTP. Note it records
  **unhandled exceptions only** — during this investigation the only entry was
  from 2026-07-09, which was itself the useful finding (nothing was crashing).

---

## How I want this done

Find the root cause before proposing a fix, and say plainly what is proven
versus inferred. The session that produced these two items burned an evening on
three confidently-wrong theories in a row — radio contention on the bridge, a
single-slot backoff bug, and then my own code — each abandoned only when
evidence contradicted it. The bridge was eventually exonerated outright by a
30-second `mock_watch.py` run that should have happened first.

So: gather evidence before theorising, prefer a cheap decisive test over a
plausible argument, and if you cannot prove something on hardware, report it as
unproven rather than rounding it up to done. Both of these items exist *because*
that rule was followed; please keep it.

Update `docs/HANDOFF.md` with whatever you find, including negative results.
Do not commit or push without being asked. `.FIT` files are git-ignored on
purpose — they are real recorded activities carrying heart rate, timestamps and
a device serial, and this repo is public.

---

## Outcome — 2026-08-03 (late session)

Both tasks worked. No hardware was available (bridge not on the USB bus, no
watch attached), so each item below is split into proven / unproven exactly.
Full detail: `docs/HANDOFF.md` "2026-08-03 (late)" entry and Known gaps.

### Task A — root cause found in the SDK; arithmetic proven on host; semantics redesigned

**Proven.** The never-observed non-zero value was structural, not a counting
bug: `app_usbd_cdc_acm_write()` returns `NRF_ERROR_INVALID_STATE` whenever DTR
is clear (`app_usbd_cdc_acm.c:943-948`), so with no tty attached *every*
console byte took the refused-write path and the counter climbed constantly —
and the `PORT_OPEN` reset zeroed it at the only moment it became readable. The
counter mostly measured deliberate drain-to-nowhere and the reset destroyed
the rest. The reset-on-open was therefore wrong, but so was the counting rule
it was compensating for; both changed together:

- FIFO + accounting extracted to `core/console_tx_fifo.c` (the
  `connect_backoff.c` pattern); `firmware/usb_cdc_log.c` keeps only critical
  regions, the staging buffer, and the USB calls.
- New rule: drops count **only while the port is open**, cumulative since
  boot, never reset. "Did I lose output before I attached" is unanswerable by
  any counter (pre-attach output is always lost in full, by design); the
  counter now answers "did I lose output during any attached session".
- `test/host/test_console_tx_fifo.c` (9 tests, in `make host-test`) forces
  every drop path: exact tail count on partial overflow, byte conservation
  (delivered + dropped == offered), wrap-around, port-state gating,
  reopen-preserves-history.

**Unproven:** the PORT_OPEN/PORT_CLOSE/TX_DONE wiring end-to-end on hardware.
Protocol (no special build): flash this tree, open the tty, write ~100
newline-separated `STATUS` commands in one burst — replies are generated
faster than TX_DONE drains, so the 1 KB FIFO must overflow while open; a later
`STATUS` must show `tx_drops` > 0 and the value must survive a tty reopen.

### Task B — fix implemented in both projects within the stated constraints; not on hardware

Root cause confirmed by reading both files — as this brief states, plus one
strengthening fact: the `startScan()` in the `pairDevice()` catch was
*unconditionally* a no-op (`mScanning` is always still true there), so
removing it loses nothing. The fix:

- The catch only sets `mRescanPending`. No BLE call from any BLE callback.
- A one-shot `tick()` consumes the flag from plain timer context —
  `compute()` in the data field; the views' 1 s timers in the ctrl app
  (`StatusView` gained one; it had none, and it is the screen a user stares
  at while the scan is dead). It calls `setScanState()` at most once per
  wedge, and only after the pending OFF callback has landed.
- Fail safe: if `tick()` never runs or the throw persists, behaviour is
  today's dead scan, nothing worse.
- Also done (was "in scope to decide"): the field's bottom row now shows
  `PAIR` (pairing requested — the state the old display mislabelled `CONN`)
  and `CONN` only after CONNECTED fires (`isLinkUp()`). The `_maybeSend()`
  write guard deliberately still uses the loose `isConnected()` — tightening
  it would make frame delivery depend on the CONNECTED callback firing, a new
  failure mode for no gain.

Both projects pass `monkeyc -l 2` (stamp `0803-2102`). **Not sideloaded, not
hardware-verified.** Test protocol: reboot the watch first, sideload, read the
stamp off the field, arm the wedge (kill a run before `onStop()` so a stale
pairing keeps `pairDevice()` throwing), start a new activity, expect `--` to
recover to `SCAN` within a few seconds instead of staying dead. Reboot between
every attempt.

Also corrected while updating HANDOFF: its Test status claimed the watch runs
`0803-1918` "with the scan-wedge fix" — that build was the broken attempt; the
watch actually runs the `0803-1932` revert.

Nothing committed. Gates at end of session: `make host-test` all green (11
host suites + UUID contract + 4 mock suites), `make firmware` links clean
(102728 text / 844 data / 16848 bss), both CIQ builds clean.
