# Handoff — 2026-07-31

**Everything below the "Historical" divider is a prior session's log, kept for
its debugging reference material. This top section is the current state.**

## What this is

An nRF52840 (Seeed XIAO, SoftDevice S340) bridge that lets a Garmin watch drive a
treadmill: a Connect IQ data field sends the current structured-workout step to
the bridge over BLE, the bridge translates it into FTMS or iFit belt commands,
and broadcasts back as an ANT footpod so the watch records real pace.

## Quick start

```sh
make host-test     # everything that runs without hardware (see Test status)
make firmware      # build the nRF52840 image
make mock-bridge   # macOS BLE peripheral that impersonates the bridge
make usb-kick      # make the app's USB console node appear (needed after EVERY boot)
```

Flashing, wiring and the SWD/DFU gotchas are in `docs/flashing.md`. A fresh clone
needs `firmware/ant_network_key.h` and `firmware/ant_license.mk` copied from
their `.example` files or the build fails — see `CLAUDE.md`.

**Flashing right now: SWD is dead, use USB-DFU.** `pyocd` cannot read the DP
IDCODE at any clock or connect mode, so the Pico↔XIAO SWD wiring needs
re-seating before `flash-full` will work again. No probe is needed in the
meantime — `docs/flashing.md` §8b is the full no-SWD recipe, and it is how the
2026-07-31 image was flashed.

## Current state — one line

**The speed-target path is fixed and fully proven against the mock: the data
field emits clean speed frames (`flags=0x01`), the belt command fires, the
keepalive holds, and pause/resume/end all behave.** Rest steps now drop the belt
to a 4 km/h walk instead of holding work pace (see Known gaps); that change is
green on host + mock but **not yet flashed or run on hardware.** Root cause of
the earlier speed-target blackout was a Monkey C
type throw (`durationValue` is a Long; `_u32` assigned Long→Byte) that fired
after the frame was built, so the old catch discarded it and every speed step
arrived all-sentinel. See
`docs/superpowers/test-logs/2026-07-31-fix-speed-target-flag-bits.md`.

**2026-08-01: the first real SDM:TGT interval run is recorded and scored.**
`test/23806153959_ACTIVITY.fit` (10 × 10 s/10 s/10 s) is graded by
`test/pace_lag_report.py` / `make pace-test`. Headline: every commanded speed
change arrived, mean response lag **2.26 s**, and five speed samples were lost
to a page-schedule artefact. Four fixes are in the tree for both (two watch-side
for the lag, two ANT-side for the dropouts) — all green on host/mock/monkeyc,
**none run on hardware yet**. Full analysis and the expected post-fix numbers:
`docs/pace-lag-analysis.md`.

**2026-08-03: all four pace fixes are CONFIRMED on hardware.**
`test/23842067586_ACTIVITY.fit` (baseline `test/baselines/23842067586-post-fix.json`)
vs the 2026-08-01 pre-fix trace:

| Metric | 2026-08-01 pre-fix | 2026-08-03 post-fix |
|---|---|---|
| effective lag | 4.393 s | **1.371 s** |
| transient (real response delay) | 2.234 s | **1.322 s** |
| edge lag mean / median / max | 2.26 / 2.0 / 5.0 s | **1.41 / 1.0 / 3.0 s** |
| best global shift τ* | 2.0 s | **1.0 s** |
| dropout IAE | 6.31 m | **0.00 m** |
| telemetry holes | 5 (2 recorded as *zero*) | 4 (**all "missing", none zero**) |
| hole clustering | **0.984 — page schedule** | 0.50 @16 s / 0.38 @17 s — **not clustered** |

Both ANT fixes land exactly as predicted: the page-2 use-state fix removed the
false *zero* speed samples (2 → 0) so dropout area is now 0.00 m, and the
background-page spread destroyed the periodicity (0.984 → not clustered at
either cycle length). The 4 remaining holes are ordinary RF loss, not the
bridge. Watch-side, edge lag landed at 1.41 s, mid-range of the predicted
1.3–1.8 s.

⚠ **Do not compare IAE totals across these two runs** — the workouts differ.
Pre-fix rested at work pace (Σ|Δ| ≈ 3.2 m/s over 19 changes); post-fix rests at
4 km/h (Σ|Δ| ≈ 39 m/s over 29 changes), so absolute area is ~12× larger by
construction. `effective_lag` = IAE/Σ|Δ| is the comparable metric and exists for
exactly this reason.

Also confirmed: **rest steps drop to 4 km/h on real hardware** — the scorer
auto-detects `walk` with `hold` fitting 3.9× worse (the pre-fix trace detects
`hold` with `walk` 14.8× worse). And per-transition area equals |Δ|·τ to within
1% on every step, i.e. the trace is pure transport delay with no mechanical
ramp, which confirms the run really was in SDM:TGT mode.

**2026-08-03: the data field stopped connecting to the bridge — investigation,
no fix flashed yet.** Ruled out first, by direct check rather than inference:
the watch-facing peripheral is *byte-identical* to the last known-good run
(`git diff 31701d4 -- firmware/ble_ctrl_svc.c firmware/app_config.h
firmware/main.c` is empty), `make check-uuid` passes all four consumers,
advertising still carries the 128-bit UUID in the primary ADV packet
(`ble_ctrl_svc.c:569-581`), link counts are `PERIPHERAL 1 + CENTRAL 1 = TOTAL 2`
(`app_config.h:23-25`, *not* exhausted), scan duty is 20 ms/100 ms, the
`BLE_GAP_EVT_CONNECTED` role filter is correct (`ble_ctrl_svc.c:313`), and the
`.prg` was built for the right device. **No code change on either side explains
it.**

The user's own observation — it fails at a location where FTMS/iFit machines are
always in range — points at the **single-slot failed-attempt backoff**
(`ble_central.c:149-152`). `s_fail_addr[6]` tracks exactly *one* address, and
`attempt_failed()` overwrites it whenever a different address fails. With two or
more unusable machines in range whose RSSI ordering alternates (and
`connect_policy_choose` is pure `best_rssi`), tracking thrashes A→B→A, so
`backoff_blocks()` returns false every time and the escalation history is
destroyed on each swap. The backoff never engages, and the bridge sits in the
continuous connect/fail/retry loop that `f52db31` was written to stop — the loop
this file's own comments (`ble_central.c:126-130`, `519-520`) describe as
keeping "the radio busy enough to starve the watch link into a supervision
timeout."

That also explains why 2026-08-01 worked: it ran in **SDM:TGT mode**, where
`80f01c8` suppresses auto-connect entirely, so the central never attempted a
connection and the watch had the radio to itself. This is the M4.2 Part B
concurrency row that has never been proven (next step 1).

**Decisive discriminator, already logged — read the USB console during a
failure:** repeated `central: attempt 1 never became usable` with *different*
device names confirms the thrash. Escalating counts (`attempt 1, 2, 3 …`) on one
name means the backoff is working and this is not the cause.

**RESOLVED 2026-08-03: the watch's Connect IQ BLE stack was wedged. A watch
reboot clears it; reinstalling the app does not.** After a power-cycle the field
came straight back to `CONN` with a live target pace, on the *same* `.prg`
(`0803-1932`) that had shown `--` minutes earlier. Since that build is
functionally the Aug-2 code that worked that morning — the only differences are
`BuildInfo.mc` and a string concat in `onUpdate`, neither touching BLE — the
fault was never in the code. It was device state.

The state survives app reinstall, which is what made it so confusing: the stamp
proved a *new* build was running and the symptom persisted anyway. It also
retro-explains the original "stopped connecting since Friday night" report —
repeated test runs, an app that dies without reaching
`onStop()`→`shutdown()`→`unpairDevice()`, and a BLE state that stays stuck until
something power-cycles the watch.

**Diagnostic order for any future "the field won't connect":**
1. Read the **build stamp** off the field — is the code you think you shipped
   actually running?
2. **Reboot the watch.** Cheap, and it clears this whole failure class.
3. Only then look at the bridge — and prove it with
   `uv run --script test/mock/mock_watch.py`, which finds and connects to it from
   the Mac in seconds.

**Still unfixed:** the app has no recovery path of its own. See Known gaps — the
2026-08-03 attempt at one made things worse and was reverted.

**Update — the bridge is exonerated.** On 2026-08-03, with the bridge holding a
live iFit link to `I_TL`, `test/mock/mock_watch.py` (which filters on the same
128-bit UUID the CIQ app uses) found `TMILL-CTRL`, connected, subscribed, and
received `S connected proto=iFit name='I_TL'`. Concurrent peripheral + central
works, advertising is correct, and the ctrl service is fully functional **while a
treadmill is connected**. So neither the backoff thrash nor radio contention
explains the watch failure — both firmware hypotheses above are dead as
*causes*. The single-slot backoff was a real latent defect all the same, and is
now **fixed** — see below.

The fault is watch-side. The field reads `--`, which means *not connected and
not scanning* — `mDevice` is null, so the scan itself never comes up. The
attempted fix for that made it worse and was reverted (see Known gaps). Next
evidence needed: `GARMIN/APPS/LOGS/CIQ_LOG.YML` off the watch, which records
`BLE profile register status=`, `BLE scan start failed:`, and `BLE pair failed:`
and distinguishes the three remaining candidates (profile registration failing,
`setScanState` throwing, or `compute()` not running).

## Test status

`make host-test` — **all green** as of the fix commit:

| Gate | Count | What |
|---|---|---|
| `make check-uuid` | 4 consumers | firmware / mock / both CIQ projects agree on the A6ED base |
| `test/host` | 10 suites | `ftms_parse` `ftms_devlist` `ifit_parse` `ctrl_dispatch` `ant_sdm_encode` `ifit_fsm` `connect_policy` `connect_backoff` `ctrl_frames` `workout_ctrl` |
| `test/mock` | 4 suites | `test_workout_probe` `test_wkt_decode` `test_link_state` `test_script_header` |

`make firmware` links clean (102424 text / 844 data / 16016 bss).

`make pace-test` is a **separate** gate (not part of `make host-test` — it needs
`uv` and a recorded .FIT). It runs the scorer's own self-test against synthetic
signals with known answers, then grades `test/23806153959_ACTIVITY.fit` against
`test/baselines/23806153959-pre-fix.json`. Both green.

⚠ `test/23806153959_ACTIVITY.fit` is **untracked on purpose** — it is a real
recorded activity (heart rate, timestamps, device serial). `make pace-test`
needs it; decide whether to commit it before publishing anything.

**Currently flashed: `4932511` + the uncommitted working tree** (`TESTBOARD=1`),
pushed over USB-DFU on **2026-08-03 19:2x** — so it includes the spread ANT
background pages and the page-2 use-state fix. Route: `make usb-kick`, then
`DFU` on the console (SWD is still down), then
`make flash-dfu SERIAL=/dev/cu.usbmodemC1B06A58A6371`; `nrfutil` reported
`Device programmed.` Verified running afterwards: product name back to
`Garmin Treadmill Bridge`, `STATUS` answers, and the heartbeat restarted at
`alive 11` and climbed monotonically — a genuinely fresh boot with `.bss`
zeroed, not the bootloader and not a stale RAM log.

**Watch data field: build stamp `0803-1918`**, sideloaded 2026-08-03 (MTP file
id 16779891). It carries the scan-wedge fix and the build stamp itself. **Read
the stamp off the field's bottom row before trusting any result** — if it does
not say `0803-1918`, the install did not take and you are testing old code.

Note the pre-flash console check also caught the bridge sitting
`connected:true, name:"I_TL"` with `LIST` showing that single iFit machine — the
state the watch-connect failures are reported in. A *connected* link does not
thrash the backoff, so if the watch still fails to connect against this build,
the mechanism is contention from an established treadmill link rather than the
connect/fail/retry loop.

## Architecture

| Path | Responsibility |
|---|---|
| `core/` | Platform-agnostic protocol logic. Compiles for the host with **no** nRF/SoftDevice/BLE includes — invariant, see `CLAUDE.md`. Parsers, FSMs, belt policy, ANT SDM encoding. |
| `core/workout_ctrl.c` | The belt-control decision: 15-byte frame in, `ACT_NONE`/`ACT_SPEED`/`ACT_STOP` out, plus dedup, the 4 km/h rest-step walk, and a ~30 s keepalive re-assert. |
| `core/machine.h` | Facade that auto-detects FTMS (`0x1826`) and iFit (`0x1533`) into one device list and routes connect/speed/stop. |
| `firmware/` | nRF5 SDK + S340 glue: BLE peripheral (watch-facing), BLE central (treadmill-facing), ANT master, USB-CDC console. `app_state.h` is the shared struct all three radios render from. |
| `watch/garmin_data_field/` | **The main product path.** Packs the workout step into 15 bytes and writes it to `A6ED0004`. (Was the site of the speed-target bug fixed on 2026-07-31 — no longer outstanding.) |
| `watch/garmin_ctrl_app/` | The SCAN/CONNECT device picker over `A6ED0002`/`0003`. |
| `test/mock/mock_bridge.py` | macOS BLE peripheral impersonating the bridge, for watch work without hardware. |
| `test/mock/workout_probe.c` | ctypes shim that compiles **the real `core/workout_ctrl.c`** into `libworkout_probe.so`, so the mock's predicted belt action is the firmware's own, never a Python reimplementation. |

## What works

- **Speed target end to end** — clean `flags=0x01` frames with full target +
  duration, `ACT_SPEED` from the real `workout_ctrl.c`, 30 s keepalive,
  pause→`ACT_STOP`, resume→re-command, end-of-activity → belt untouched.
  Acceptance rows 3–6 all PASS against the mock (2026-07-31).
- **Three-radio concurrency** — the core architectural risk v2 existed to test.
  BLE peripheral + BLE central + ANT master held ~366 s with zero faults, belt
  tracking target, on a **real iFit treadmill**. See
  `docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`.
- **Belt speed control** — 8.00 km/h commanded → 7.99 km/h measured.
- **Watch ⇄ bridge BLE transport** — the watch finds the service, connects, and
  writes 15-byte frames that decode correctly on the wire. Verified 2026-07-31
  against the mock.
- **The mock rig itself** — decode, link inference, and firmware-sourced belt
  prediction all behaved through a full workout.
- **SDM cadence is broadcast as invalid, not zero** (`d5782ac`, 2026-07-31).
  Page 2 previously sent `0x00` integer + `0x0` fraction, which in the SDM
  profile is a *valid* reading of 0 strides/min rather than "no data" — so the
  watch trusted the footpod and recorded a flat 0 spm for the whole run instead
  of falling back to its own wrist cadence. Now `0xFF` in the integer byte and
  `0xF` in byte 4's high nibble (`core/ant_sdm_encode.c`); the speed integer
  still occupies that byte's low nibble, so only the cadence half changed.
  Asserted by `test_ant_sdm_encode`. ⚠ **Flashed but not yet observed over the
  air** — no ANT receiver on the dev machine; confirm from a recorded .fit.
- **SDM target-broadcast debug mode** — testboard button action `SDM:TGT`
  makes the ANT footpod broadcast the commanded target speed
  (`resolved_target_mps`, distance integrated from it) instead of the actual
  belt speed, so a watch-recorded .fit captures exactly what the bridge
  commanded. Run the same workout in normal mode for the actual belt trace
  and diff the two .fits to score accuracy. OLED `A:T` + label `SDM:TGT` show
  the mode. Time and distance are integrated from the app_timer RTC, so it
  works with **no treadmill connected** — the target still latches, because
  `machine_set_speed()` writes `resolved_target_mps` before its connection
  check. ⚠ Built and flashed 2026-07-31, **not yet exercised on hardware**:
  the toggle is a physical button press and there is no ANT receiver on the
  dev machine, so nobody has watched a broadcast yet.

## What was broken — now fixed (2026-07-31)

**The data field never emitted a speed target.** Across the original full
structured workout, all 13 frames arrived with `flags=0x00` and not one carried
`tgt=0(SPEED)`. Root cause, found with diagnostic flag bits (`FLAG_SRC_*` in
`DataFieldView.mc`, ignored by the bridge): `_packFrame()` threw **after**
building a complete frame — `wStep.durationValue` is a Long on pace-target
steps and `_u32`'s Long→Byte byte writes throw in Monkey C — and the old
`catch` discarded the frame, so every speed step went out as an all-sentinel
base frame. Fix: `dv.toNumber()` coercion, plus keeping the partial frame in
the catch (marked with bit `0x02`). Full story:
`docs/superpowers/test-logs/2026-07-31-fix-speed-target-flag-bits.md`.

The 2026-07-30 "inconsistent timing" note was already retracted — it was never
a timing bug; `mWritePending` is exonerated. The earlier "moving to another
interval with a different target pace didn't always work" report is fully
explained — it never worked, until now.

## Next steps, in priority order

0. ~~**Flash + sideload the 2026-08-01 pace fixes and re-run the scoring
   workout.**~~ — **DONE 2026-08-03.** All four fixes confirmed on hardware; see
   the table in Current state. New reference trace
   `test/23842067586_ACTIVITY.fit`, baseline
   `test/baselines/23842067586-post-fix.json`.

   `make pace-test` now **defaults to this post-fix pair**, with `SDM_CYCLE`
   empty so the page cycle is read from `firmware/ant_sdm.c` (16.00 s today) and
   stays correct as that file changes. The pre-fix trace is still scorable, but
   only with its own recording firmware's values passed explicitly:
   `make pace-test FIT=test/23806153959_ACTIVITY.fit BASELINE=test/baselines/23806153959-pre-fix.json SDM_CYCLE=17.0`
   — both invocations verified PASS on 2026-08-03.
1. **M4.2 Part B hardware rows (B1/B4/B5)** — real Garmin watch → real bridge
   → real treadmill: watch pace tracks belt speed with the treadmill link up
   (footpod pairing proved ANT alone, never concurrently). See
   `docs/finishing-plan.md`. The mock-side acceptance table (rows 3–6) is now
   fully proven; Part B is the remaining gate.
2. **Stale-reset on watch disconnect** (acceptance row 7 territory):
   `workout_ctrl_reset()` on link loss is implemented — confirm the belt stops
   when the watch walks away mid-run, on hardware.
3. ~~**DFU ctrl command end-to-end** (`b4c3845`)~~ — **PROVEN on hardware
   2026-07-31.** `DFU` over the USB console rebooted the board into the Secure
   DFU Bootloader, `make flash-dfu` reported `Device programmed.`, and the app
   came back up. Exercised twice. This is now the primary flashing route (SWD
   is down); see `docs/flashing.md` §8b.
4. ~~**Exercise `SDM:TGT` on hardware — nobody has watched a broadcast yet.**~~
   — **DONE 2026-08-01.** `test/23806153959_ACTIVITY.fit` is a full 5-minute
   SDM:TGT run: the watch paired with the footpod, and pace *and* distance both
   advanced for the whole run with no treadmill connected (the analyser confirms
   distance tracked the commanded speed to within 0.1% even across the telemetry
   holes). What that run also exposed — a 2.26 s response lag and a periodic
   speed dropout — is item 0 above and `docs/pace-lag-analysis.md`.
   Press button action 4 (label flips `SDM:ACT`→`SDM:TGT`, OLED row 0 shows
   `A:T`, 2400 Hz chirp on / 700 Hz off). Also confirm the bridge no longer grabs
   a treadmill on its own while the mode is on (see below).

   ~~Auto-connects to a treadmill during standalone testing~~ — **fixed
   2026-07-31.** `policy_evaluate()` and `ble_central_autostart()` bail out
   while `sdm_broadcast_target` is set. Manual `CONNECT` still works, and an
   existing link is deliberately not torn down (that would stop a belt
   someone is running on). The action label also changed from a state readout
   to `SDM:>TGT` / `SDM:>ACT` — it sat in the row that names the *next
   action*, so reporting current state read as the exact opposite.

   ~~Does not work with no treadmill connected~~ — **fixed 2026-07-31.** Time
   and distance were integrated from `treadmill.elapsed_s`, which is populated
   only by FTMS treadmill-data notifications (`core/ftms_parse.c:41`) and
   zeroed on disconnect (`firmware/ble_central.c:685`), so with nothing
   connected the delta was always 0 — distance flat, page-1 time field pinned
   at 0. Both now integrate from the app_timer RTC on every TX event
   (`firmware/ant_sdm.c`), which keeps running regardless of the belt link.
5. **Confirm the invalid-cadence encoding on a recorded run** — flashed in
   `d5782ac` but never seen on the air. Record any run with the footpod paired
   and check the cadence field in the resulting .fit: it should now carry
   wrist-derived cadence instead of a flat 0 spm. If a watch ignores the
   `0xFF`/`0xF` invalid encoding, the fallback is to transmit **SDM capabilities
   page 22 (0x16)**, whose bitfield flags cadence as unsupported explicitly — we
   do not send page 22 at all today, and adding it means a new slot in the
   68-slot TX cycle in `firmware/ant_sdm.c`.
6. Cosmetic: pause reports `timer=1(STOPPED)`, never `2(PAUSED)` on this
   watch. Both stop the belt; not worth chasing.
7. **Score belt accuracy on hardware** — with the new `SDM:TGT` debug mode
   (button action 4), run the same workout twice on the real watch/treadmill
   (normal vs target broadcast) and diff the .fit speed traces. This path *is*
   sound with a treadmill connected; only the no-treadmill case is broken
   (item 4).
8. **Re-seat the SWD wiring** (Pico GP2→SWCLK, GP3→SWDIO, GND→GND). Not
   urgent — USB-DFU covers routine flashing — but `flash-full` is the only
   recovery path if the app is ever left invalid.

All code milestones (M0–M4.2 core) are complete; only the physical-hardware
concurrency gate remains, per `docs/finishing-plan.md`.

## Known gaps and non-goals

- **Failed-attempt backoff is now per-address — FIXED 2026-08-03, built but not
  yet exercised on hardware.** It tracked a single address, so it did nothing
  whenever more than one machine was misbehaving: `attempt_failed()` overwrote
  the slot on every *different* address, destroying the previous one's history,
  and `backoff_blocks()` only matched the one address it happened to hold. With
  two failing machines A and B the sequence was `fail A` → slot=A count 1,
  `fail B` → slot=B count 1, `fail A` → slot=A count 1 — nothing ever blocked
  and nothing ever escalated. Because the pick is pure `best_rssi` and RSSI
  ordering flips between nearby machines, alternating is the *normal* case in a
  room with several treadmills, not a corner case.

  The policy moved to `core/connect_backoff.c` (4-slot table, least-recently-
  failed eviction) and is host-tested — `test/host/test_connect_backoff.c`
  covers the alternating-address regression directly, plus the escalation
  schedule, expiry-preserves-count, per-address success, eviction, count
  saturation, and 2^32 clock wrap. `firmware/ble_central.c` keeps only the
  clock and the call sites.

  Two deliberate behaviour changes: success now forgives **only** the address
  that succeeded (clearing the whole table would let a working treadmill forgive
  a broken one, and the next disconnect would hammer it again with no cooldown);
  and the firmware feeds core a free-running ms clock accumulated from the RTC,
  since the 24-bit counter wraps every ~512 s and cannot be passed as a
  timestamp.

  ⚠ This was **not** the cause of the watch-connect failure — the bridge was
  exonerated before this was written. It is a fix on its own merits, and the
  loop it prevents is still worth preventing.

- ⚠⚠ **The "scan-wedge fix" was attempted 2026-08-03 and REVERTED — it broke the
  field outright. Do not re-apply it as written.** The *analysis* still looks
  right: both CIQ projects use `mScanning` (the *observed* radio state, written
  only from the `onScanStateChange` callback and therefore always one callback
  behind) as the guard for *intent*, so a `pairDevice()` throw runs
  `startScan()` while `mScanning` is still `true`, it early-returns, the OFF
  callback lands, and nothing restarts the scan. **But the attempted fix was
  worse than the disease.** It split the state into `mScanning` + `mWantScan`,
  reconciled by calling `setScanState(SCANNING)` *from inside the
  `onScanStateChange` callback*, plus a 1 Hz `startScan()` backstop from
  `compute()`. Measured on hardware (build `0803-1918`): the field went from
  `CONN` + live target pace to **`--` with no pace at all**, and the bridge
  testboard showed `W:-` — no watch link, no scan, and `compute()` apparently
  not running either. Reverted in build `0803-1932`, which restores the exact
  pre-2026-08-03 scan logic.

  **Why it broke: issuing a BLE operation from within a BLE callback.** Connect
  IQ does not tolerate re-entering the BLE stack from `onScanStateChange`, and
  the 1 Hz `setScanState` hammering from `compute()` compounds it. Confirmed by
  elimination: reverting restored the code but *not* the behaviour, and only a
  watch reboot did — i.e. the bad build had wedged the CIQ BLE stack into a
  state that survives app reinstall.

  Any future attempt must re-arm the scan from a **deferred** context (a flag
  set in the callback and consumed by the next `compute()`), never from inside
  the callback itself; must not call `setScanState` more than once per state
  transition; and must be verified on hardware by reading the build stamp off
  the field. Budget a watch reboot between attempts — without one you are
  measuring the previous attempt's wreckage, which cost most of an evening on
  2026-08-03.

  **Workaround until then: reboot the watch.** It is the only thing that clears
  a wedged BLE state, and it fixes the user-visible symptom completely.
- **`isConnected()` is `mDevice != null`, and `pairDevice()` sets `mDevice`
  before the link is up** (`CtrlBleDelegate.mc`). The field can therefore
  display `CONN` while nothing works. Not fixed — but worth knowing when reading
  the field during a failure: `CONN` is "pairing requested", not "link up".
- ⚠ **A free run does not move the belt, by design.** `decode_action()` returns
  `ACT_NONE` with no structured speed step, meaning "don't touch the belt". This
  has looked like a bug twice. It isn't. *Starting* the belt *requires* a
  structured workout with a speed target.
- **Rest steps drop to 4.0 km/h** (`REST_SPEED_KMH`). A rest step arrives as
  `intensity=1(rest) tgt=2(OPEN) flags=0x00` — no speed target — and before
  2026-07-31 that hit the `ACT_NONE` "hold" path, so the belt ran the work
  interval speed straight through the rest. It now commands a 4 km/h walk. An
  explicit speed target on the rest step still wins. Because the watch has been
  seen reporting warmup and cooldown as `intensity=1` too, those get the same
  4 km/h walk; that is accepted, not a bug. Design:
  `docs/superpowers/specs/2026-07-31-rest-step-rest-speed-design.md`.
- `main` is published to `origin` and in sync as of 2026-07-31. (An earlier
  version of this line claimed nothing had ever been pushed — stale.)
- **One treadmill connection at a time** is an invariant, not a limitation —
  never reintroduce simultaneous FTMS + iFit.
- The mock does **not** emulate the ctrl grammar (`A6ED0002`/`0003`, `SCAN`/
  `CONNECT`/`LIST`, `D`/`E`/`S` frames). `garmin_ctrl_app` is not exercised by it.
- The mock's `LINK` lines are **inferred from write traffic**, not reported by
  the BLE stack: bless's `is_connected()` tracks *subscribed* centrals and the
  data field never subscribes, so it reads False forever. A legitimate 35 s
  inter-frame gap was observed — do not tighten the 120 s stale window.
- `intensity` 1/2/3 → rest/warmup/cooldown is read from alignment with the
  workout that was run, **not** from a firmware-pinned constant table. Only
  `SPEED=0` for targetType is grounded in this repo's source.
- Open GATT server (no pairing) is an accepted, recorded risk.
- `make flash-app` is broken (parks in the bootloader); use `make flash-full`.
  `make flash-sd` chip-erases and destroys USB-updatability.

## Dependencies and tooling

- **Host tests:** system `cc` and `python3`. No third-party packages.
- **Mock bridge:** `uv` (the script is a PEP-723 `uv run --script`), which pulls
  `bless==0.3.0`. The pin is load-bearing — the advertising behaviour the design
  rests on (`prioritize_local_name`, and the `len(name) > 10` rule that drops
  service UUIDs from the advert) is a bless implementation detail, not a
  contract. `test_script_header.py` enforces that the pin stays exact.
  ⚠ **Power the real bridge off** before running the mock, or the field will
  pair with whichever it finds first and you will debug the wrong peer.
- **Firmware:** PlatformIO's GCC **7.2.1** (not Homebrew's), nRF5 SDK 17.1.0,
  S340 v7.0.1 API headers. Concrete paths in `CLAUDE.md`.
- **Flashing:** no onboard debugger — SWD via a Pico/CMSIS-DAP + `pyocd` (not
  `nrfjprog`), USB via `nrfutil`.
- **Watch:** Connect IQ SDK for rebuilding `watch/`; sideload the `.prg` over
  **MTP** with libmtp (`mtp-sendfile`) — the fenix 8 has no mass-storage mode,
  `/Volumes/GARMIN` never mounts. Full procedure in `watch/README.md`
  §Sideloading.

---

# Historical — session log from 2026-07-29

Kept for the debugging techniques and the retracted-hypotheses record, both of
which are still valuable. **Its "State", test counts, and "Next steps" sections
are superseded by everything above** — in particular, the claim that there is no
watch-side code in this repo is no longer true: both CIQ projects were vendored
into `watch/` on 2026-07-29.

---

## State

Branch `fix/code-review-2026-07-27`, working tree **clean**, 23 commits.
`main` and `origin/main` untouched (`origin/main` still at `e68a403` — nothing
pushed).

**The M4.2 concurrency gate PASSES Part A**, and the treadmill leg passed on a
**real iFit treadmill** rather than the FTMS mock — strictly stronger than the
plan asked, since the iFit path (`0x1533`) cannot be exercised against that mock
at all. Full results and evidence:
`docs/superpowers/test-logs/2026-07-21-concurrency-gate.md`.

| Check | Result |
|---|---|
| A1 BLE peripheral (mock watch) | **PASS** |
| A2 BLE central scan + connect (**real iFit**) | **PASS** (5th attempt — see open defect 1) |
| A3 belt-speed control | **PASS** — 8.00 km/h target → 7.99 km/h belt |
| A4 ANT master (footpod #45694) | **PASS** |
| A5 all three radios, 60 s | **PASS** — ~366 s, zero faults |
| B2 real treadmill link | **PASS** (iFit, `I_TL`) |
| B3 belt-speed control, real treadmill | **PASS** |
| B1 / B4 / B5 real Garmin watch | **NOT RUN** — see "Next steps" |

**The core architectural risk v2 exists to test is cleared.** S340 running BLE
peripheral + BLE central + ANT master concurrently held for ~366 s with
`last_fault_code = 0` throughout, belt tracking target, ANT broadcasting, and
zero disconnects on either link. What remains is peer compatibility with the real
watch, not concurrency.

Gates (as of 2026-07-29, before the mock-bridge branch — see the 2026-07-30
update above for the current count): `make host-test` 9/9; firmware links
clean with `-Werror` for both `TESTBOARD=0` and `TESTBOARD=1`.

Currently flashed: `775dde7`, `TESTBOARD=1`, via `make flash-full`. Boots clean.
Note `flash-full` chip-erases, so **FDS is wiped and there is no saved
last-device** — the bridge will not auto-connect until one `SCAN` + `CONNECT`.

Hardware: XIAO nRF52840 + Pico CMSIS-DAP probe (GP2→SWCLK, GP3→SWDIO, GND→GND).

---

## Fixed this session (2026-07-29)

### The watch could never see the bridge — wrong UUID base (`775dde7`)

v2 shipped the 128-bit ctrl-service base as
`A6ED0000-2E7A-4E1D-9E3B-000000000000`. That is a **sanitization placeholder,
not the contract.** The old firmware
(`$OLD/boards/xiao-nrf52840/platform_ble_ctrl_svc.c`) and **both** Garmin CIQ
projects use `A6ED0001-D344-460A-8075-B9E8EC90D71B`.

The CIQ data field and ctrl app discover the bridge by **filtering on the
128-bit service UUID**, so a placeholder base makes the bridge invisible to the
watch — with no error anywhere to explain it. B1/B4/B5 were unpassable, which is
how it surfaced. `CLAUDE.md` compounded it by claiming the control contract was
"unchanged from old repo" while documenting the wrong UUID, so inspection alone
could not catch it.

`mock_watch.py` carried the same placeholder and discovers the bridge the same
way, so it had to move in lockstep — changing only the firmware would have
silently broken the Part A instrument.

⚠ **Do not re-sanitize this base.** It is load-bearing for watch compatibility.
`CLAUDE.md` now carries a warning to that effect.

### Nothing started scanning at boot, so auto-reconnect never worked

Reported from the bench as "it only auto connects after running scan", and that
was exactly right. The only things that ever called `ble_central_scan_start()`
were a ctrl `SCAN` from the watch, the testboard button
(`main.c` `testboard_action_scan()`), and the rescan-after-disconnect path —
which presupposes an earlier connection. A freshly powered bridge therefore sat
in `LINK_DOWN` indefinitely, and `last_device`, whose entire purpose is "remember
for next power-up", could never fire: `connect_policy` rule 1 hands the saved
device the link the moment it appears, but it never got to see an advert.

Added `ble_central_autostart()`, called from `main()` **after** all three radios
are up (so the scan competes with a fully configured ANT master rather than
starting mid-bring-up). It is deliberately gated on actually having a saved
device: the scan module runs with `NRF_BLE_SCAN_SCAN_DURATION 0` (scan until
stopped) and its radio time competes with the watch link and ANT — the exact
contention that was starving the peripheral link — so a bridge that has never
paired waits to be asked rather than scanning forever for nothing.

Logs `central: saved device "…" — scanning to reconnect`, or
`central: no saved device — idle until SCAN`.

⚠ Verifying this needs care: `make flash-full` chip-erases and wipes FDS, so the
boot straight after a flash always reports "no saved device". The real test is
SCAN + CONNECT (which saves), then **`make reset`** — not another `flash-full`.

### Watch link dropped under three-radio load (`8b99249`)

Three causes conspired to kill a healthy watch link with
`BLE_HCI_CONNECTION_TIMEOUT (0x08)` whenever the central was mid-connect. All
three verified on hardware:

1. **No PPCP advertised** → the central chose the supervision timeout
   unilaterally and macOS picked **720 ms**, far too thin for a device that also
   drives a BLE central and an ANT master. Now we advertise a preference *and*,
   because PPCP is only a hint, check what we actually got and renegotiate
   anything under 2 s:
   ```
   ctrl_svc: watch connected (handle 1) int=24 lat=0 sup=72
   ctrl_svc: sup=72 too short — requested 400
   ctrl_svc: conn params now int=48 lat=0 sup=400
   ```
   The link then survived ~670 s and ~366 s across two boots, through five
   central connect/disconnect cycles, with zero `0x08`.
2. **Scan duty cycle was the SDK default 160/80 (50%)** and the central
   connection-interval floor was 7.5 ms. A treadmill streams at 1-4 Hz, so
   7.5 ms bought nothing. Now 20% duty, 30-60 ms interval.
3. **`last_device` was saved on `BLE_GAP_EVT_CONNECTED`**, so a machine that
   accepted a link but failed discovery became "the saved device" — and
   `connect_policy` rule 1 hands the saved device the link the instant it
   reappears, forever. Saving moved to `subscribed()`. Confirmed: the connects
   that died with `0x3E` produced **no** `last_device: saving` line; only the one
   that reached `subscribed` did.

---

## Still open

### 1. ~~Unbounded tight reconnect loop on `0x3E`~~ — FIXED

Fixed with a per-address escalating backoff (1/2/4/8/16/30 s) armed on any
attempt that never reached `DISC_DONE`, and cleared in `subscribed()`. A link
that *did* become usable and then dropped is not a failed attempt, so recovery
from a genuine treadmill power-cycle stays immediate.

Two design points worth not undoing:
- The gate lives in `policy_evaluate()`, **not** `on_adv_report()`. The earlier
  cooldown filtered the advert, which also removed the device from `s_devs` —
  so it would have vanished from `LIST` and the watch could not have picked it
  manually even when a human explicitly asked. Now only the *automatic* pick is
  suppressed; the device stays visible and manually selectable.
- The backoff is armed **only** in `BLE_GAP_EVT_DISCONNECTED` (plus the connect
  timeout, which produces no disconnect). Arming in `gattc_fail()` too would
  double-count and skip a step, since every failed attempt ends in a disconnect.
  `gattc_fail()` leaving `s_stage` at `DISC_IDLE` is what signals "never became
  usable" to that handler.

Escalation verified as 1/2/4/8/16/30/30 s.

⚠ **The first version of this fix did not actually escalate**, and only a
hardware run caught it. `backoff_blocks()` cleared `s_fail_count` along with
`s_have_fail` when the window expired, so the next failure found no history and
restarted at 1. Four consecutive failures all logged
`attempt 1 … not auto-retrying for 1000 ms`. Expiry must stop *blocking* without
erasing the *history*; only `attempt_succeeded()` clears the count. The
arithmetic had been unit-checked in isolation and was fine — the state machine
around it was not, which is exactly the gap a bench run closes and a desk check
does not.

Note the base step is close to a no-op by design: `policy_evaluate()` already
runs on the 1 Hz policy tick, so 1000 ms means "retry next tick", which is right
for a transient failure. The escalation is what does the work.

Original report follows.

Observed four consecutive failures before the fifth attempt succeeded:
```
connecting to "I_TL" (iFit) → connected "I_TL" (iFit, handle 0)
  → disconnected (reason 0x3E) → scanning for treadmills... → (repeat)
```
`0x3E` = `BLE_HCI_CONN_FAILED_TO_BE_ESTABLISHED`: the link is created
(`CONNECTED` fires) but the link layer never completes establishment.

`ble_central.c` `BLE_GAP_EVT_DISCONNECTED` calls `ble_central_scan_start()` on
**every** disconnect regardless of reason, with no backoff; `policy_evaluate()`
then re-picks the saved device on the very next advert. A machine that never
establishes loops forever. The 30 s cooldown added in `8b99249` does **not**
help — it arms only in `gattc_fail()` (a GATT *discovery* failure), and `0x3E` is
an *establishment* failure on a different path.

Suggested fix: treat **any** disconnect that occurred before `s_stage` reached
`DISC_DONE` as a failed attempt, and apply an escalating per-address backoff
(1/2/4/8 s, capped at 30 s) — unifying the `0x3E` and `gattc_fail` paths. A link
that *did* reach `DISC_DONE` must still reconnect immediately, preserving fast
recovery on a genuine treadmill drop (hardware-test-plan Phase 8.1). Capture
`s_stage` before the handler resets it to `DISC_IDLE`.

Root cause of the `0x3E` itself is **not** established. Candidates: radio
contention from three concurrent radios; a stale link held by the treadmill after
the board was reset out from under it; marginal RSSI (-60 to -70 observed).

### 2. ~~Advert names arrive empty — blocks the watch device picker~~
### — MISDIAGNOSED. It was a misleading log line, not a picker blocker.

**Correction (2026-07-29).** The earlier entry here claimed `LIST` would show
blank names and the `garmin_ctrl_app` picker would be unusable. That was wrong,
and it was inferred from a log line rather than measured. The name pipeline works
end to end:

1. The primary advert carries the service UUID but usually **no** name, so it
   creates the `s_devs` entry nameless.
2. The name arrives in the **scan response**, a separate report. `nrf_ble_scan`
   sets `scan_params.active = 1`, so scan responses *are* requested — the
   existing `else if (name[0])` branch in `on_adv_report()` attaches it by
   address.
3. `ftms_devlist_upsert()` already refuses to let a later nameless advert blank a
   captured name (`if (d->name[0])`).

The evidence was there all along: `central: connecting to "I_TL" (iFit)` — the
name *was* in the list entry by connect time.

The real problem was that `central: found "%s"` fires only on first sight, i.e.
before the scan response lands, so it always printed `""` and read as "no name
available". Fixed by commenting why first sight is nameless and adding a
`central: name for idx N is "…"` line when a name is actually attached. A stale
comment claiming "we scan passively" was also corrected — the SDK sets
`active = 1` (`nrf_ble_scan.c:975`).

Residual narrow race, not worth code today: a `LIST` issued in the window
between the primary advert and the scan response would show that device
nameless. A re-`LIST` fixes it.

### 3. ~~`A6ED0004` workout-frame writes are unlogged~~ — FIXED

`wkt_log()` in `ble_ctrl_svc.c` now logs incoming workout frames, and
specifically flags **malformed** frames that `workout_ctrl_on_frame()` otherwise
drops in silence. Logged on change of the decision-relevant prefix (bytes 0..8:
version, timerState, flags, intensity, targetType, targetLow/High) plus a ~1 min
heartbeat — never every frame, which at the data field's ~1 Hz `compute()` rate
would wrap the 8 KB RTT ring and bury everything else.

This exists to separate three failure modes that look identical from the outside
and have completely different fixes:
- the watch is not writing frames at all
- frames arrive but carry no speed target (a free run — `ACT_NONE`, correct
  behaviour, belt deliberately untouched)
- frames arrive malformed and are dropped

Expect `tgtType=0` for a speed target and `tgtType=255` when the watch knows of
no structured step.

### 4. ~~USB CDC never completes enumeration~~ — SOLVED 2026-07-31

**`make usb-kick`.** Bus reset + a manual `SET_CONFIGURATION(1)` on EP0, after
which macOS instantiates the interfaces and `/dev/cu.usbmodem<SERIAL>1` appears
within a second. Needed after every app boot. Full measurement trail and the
remaining unproven root cause are in `docs/flashing.md` §8a.

Both earlier theories were wrong and should not be revived:

- **Not the cable / not the multi-packet EP0 IN.** libusb reads the device
  descriptor *and* the full 75-byte configuration descriptor cleanly. What the
  device actually stalls, right after boot, is `GET_CONFIGURATION` and
  `SET_CONFIGURATION` — one specific pair of requests, which no cable fault can
  select for.
- **Not "firmware exonerated" either.** The USB state machine reaching
  `USBD started` says the stack came up; it says nothing about whether the
  event queue was being pumped when macOS issued its single configure attempt.
  That is the live suspect (`docs/flashing.md` §8a).

The measurement that had never been taken was not a bus capture — it was simply
issuing the control transfers by hand from the host.

### 5. Smaller items

- **A ctrl reply was truncated in the log:** `ctrl: {"cmd":"sto` for the `STOP`
  reply, despite `ble_ctrl_svc.c:194` correctly using `nrf_log_push`. Sibling
  replies logged in full. Cosmetic, unexplained.
- **An unidentified FTMS advertiser with an empty name** appeared on one boot
  (`found "" rssi -67 (FTMS)`); an auto-connect to it timed out before the
  operator redirected to the iFit machine. Possibly a stray `mock_treadmill.py`
  or a neighbouring device. Cost a wasted 5 s connect during a live session.
- **`APP_ERROR_CHECK(fds_init())` is still fatal-on-error.** `f01f79d` bounded
  the *wait* so a slow init cannot hang boot, but an FDS *error* still kills the
  device. `last_device.c` already has `s_fds_unavailable` for exactly this;
  degrading gracefully would cost last-device memory instead of the product.
- **Test the `DFU` ctrl command** (`b4c3845`) end-to-end.
- Still unfixed, deliberately: `BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE` is handled
  but does not requeue dropped iFit frames.
- Open GATT server (no pairing) is an **accepted risk**, recorded in the spec.

---

## Next steps — finishing Part B (B1, B4, B5)

Everything needed is in place. **No Connect IQ rebuild is required**: the
prebuilt binaries already embed the correct `D344` UUIDs, verified with
`strings`.

1. **Sideload the data field.** The fenix 8 has **no USB mass-storage mode**
   (`/Volumes/GARMIN` never mounts) — sideload over MTP with libmtp:
   `mtp-sendfile <prg> <Apps-folder-id>`, where the numeric folder id comes
   from `mtp-filetree` (name paths and `-f` flags fail — see `watch/README.md`
   §Sideloading for the full procedure and pitfalls). `fenix8solar51mm` is
   already in the manifest's product list (alongside fr965, fr955, fr970).
   Optionally also build and sideload the ctrl app from `watch/garmin_ctrl_app`
   for the picker — but see open defect 2; names will be blank.
2. **Add the data field to a run activity's data screen.**
3. ⚠ **Load and start a structured workout with a SPEED target.** This is a hard
   prerequisite, not a nicety. `core/workout_ctrl.c` `decode_action()`:
   - `timerState != ON` (off / stopped / paused) → **`ACT_STOP`**, belt stops.
   - `timerState == ON`, no structured step (free run) → **`ACT_NONE`**, belt
     left alone **by design**.
   - `timerState == ON`, step present but target is not speed → `ACT_NONE`.
   - `timerState == ON` + speed target → `ACT_SPEED` at the midpoint of
     `targetLow`/`targetHigh`.

   So a **free run will not move the belt** and that is correct behaviour, not a
   failure. B3/B5 need an actual speed-target workout on the watch.
4. **One `SCAN` + `CONNECT`** is needed after the reflash — `flash-full`
   chip-erased FDS, so there is no saved device to auto-connect to. Once a link
   reaches `subscribed`, `I_TL` is saved and later boots auto-connect.
5. Record results in the gate log; B4 wants pace on the watch tracking belt speed
   while the treadmill link is up (the earlier footpod pairing proved ANT alone,
   never concurrently).

There is **no watch-side code in the v2 repo at all** — both CIQ projects live
only in `$OLD`. Consider vendoring them into v2 so the UUID contract cannot drift
between firmware and watch again; that drift is exactly what cost this session.

---

## Reference: how to debug this class of failure

The techniques that found everything above. All read-only, no console needed.

**1. Read device state straight out of RAM.** More direct than any log line.
`app_state_t` is `s_state`, 52 bytes. ⚠ `link_state_t` **packs to one byte** —
arm-none-eabi-gcc defaults to `-fshort-enums`:

| Off | Addr | Field |
|---|---|---|
| 0 | `0x20004614` | `treadmill.speed_mps` (float) |
| 4 | `0x20004618` | `treadmill.distance_m` (float) |
| 8 | `0x2000461c` | `treadmill.incline_pct` (float) |
| 12 | `0x20004620` | `treadmill.elapsed_s` (u32) |
| 16 | `0x20004624` | `central_link` (**1 byte**: 0=DOWN 1=SCAN 2=CONN 3=UP) |
| 17 | `0x20004625` | `treadmill_name[24]` |
| 41 | `0x2000463d` | `watch_connected` (bool) |
| 42 | `0x2000463e` | `ant_broadcasting` (bool) |
| 44 | `0x20004640` | `resolved_target_mps` (float) |
| 48 | `0x20004644` | `last_fault_code` (u32) |

Addresses move when the build changes — re-derive with
`nm -S _build/nrf52840_xxaa.out | grep ' s_state'`.

**2. Read the log out of RAM over SWD.** The RTT backend buffer is a plain array:
```sh
nm -S _build/nrf52840_xxaa.out | grep _acUpBuffer      # e.g. 2000593c
pyocd commander -t nrf52840 -O connect_mode=attach -c "read8 0x2000593c 2200"
```
It is **8 KB** now (was 512 B) and `RdOff` stays 0 with nothing draining it, so a
whole boot fits and stays readable. Track `WrOff` between reads to isolate only
new output. Control block `_SEGGER_RTT`: `aUp[0]` at `+0x18` — `pBuffer +0x1C`,
`SizeOfBuffer +0x20`, `WrOff +0x24`, `RdOff +0x28`. Drain by hand with
`write32 <RdOff_addr> <WrOff_value>`.

At 512 B the boot log filled at `ant_sdm init` and the backend's non-blocking
*skip* mode silently discarded everything after it — which is why fatal errors
were invisible for so long.

**3. `RESETREAS` (`0x40000400`) tells a crash from a button press.** `0x1` =
RESETPIN (operator or probe), `0x2` = watchdog, `0x4` = soft reset
(`NVIC_SystemReset` — testboard long-press or a fatal error), `0x8` = lockup.
A power-on reset reads `0x0`. It **accumulates and is never cleared in
software**, so read it as "what has happened since power-on". This immediately
distinguished an operator reset from a concurrency failure this session.

**4. Get file:line on fatal errors by defining `DEBUG`.** Without it,
`app_error_fault_handler` logs only `"Fatal error"`. With it you get
`ERROR <code> [<name>] at <file>:<line>`. Temporarily add `CFLAGS += -DDEBUG` to
`firmware/Makefile`. Note `DEBUG` also suppresses the `NVIC_SystemReset()` on
error, so the board parks instead of reset-looping.

**5. Decode SDK error codes.** `34314` = `0x860A`; `NRF_ERROR_FDS_ERR_BASE` is
`0x8600` (`sdk_errors.h:96`), offset 10 in the `fds.h:85` enum =
`FDS_ERR_NO_PAGES`.

**6. Locate a fault.** `halt`, `reg pc`, then `addr2line`. Read `HFSR`
(`0xE000ED2C`) and `CFSR` (`0xE000ED28`): `HFSR=0x80000000` is DEBUGEVT (a
`BKPT` executed — an assert, not a bad pointer). The stacked exception frame at
MSP gives the faulting PC and its caller.

**7. ⚠ Do NOT `halt` while the SoftDevice is running.** Halting breaks the SD's
radio/timeslot timing and trips an assert, so the board dies a few seconds later
*because you looked at it*. A halt-every-4s poll once made a healthy app appear
to die at ~11 s. Plain `read8`/`read32`/`write32` in `connect_mode=attach` do
**not** halt and are safe — poll a counter instead. Confirmed again this session:
many `attach`-mode reads left the heartbeat perfectly monotonic. Use `halt` only
on a board that has already faulted.

**8. Run the host mocks from a real terminal.** macOS denies Bluetooth to a
process with no controlling TTY, and the failure is **silent** — the mock prints
"advertising" and is genuinely on air, just invisible.

**9. ⚠ macOS caches a peripheral's GATT database and does NOT invalidate it when
the firmware changes.** The cache is keyed by device address. Advertisements are
read live off the radio, but the service/characteristic list served on connect
can be stale — so after the UUID change in `775dde7` the host matched the **new**
service UUID in the advert, connected, and then failed to find the **new**
characteristics:
```
found: … TMILL-CTRL
connected: True
BleakCharacteristicNotFoundError: Characteristic a6ed0003-d344-… was not found!
```
The device side is blameless in this signature. Confirm it by reading the RTT
log: you will see `watch connected`, the conn-param renegotiation, then
`watch disconnected (reason 0x13)` — `0x13` is
`BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION`, i.e. *the host* hung up — and **no**
`notifications on`, because the CCCD was never written.

Fix: toggle Bluetooth off/on in System Settings, or `sudo pkill bluetoothd`.

`test/mock/dump_gatt.py` diagnoses it. It finds the bridge **by name** rather
than by service UUID, so it works no matter which base the host currently
believes in, dumps every service and characteristic, and prints a verdict
distinguishing a stale host cache from a genuine firmware problem.

Related trap in the same family: **a connected peripheral link stops
advertising**, so a mock left running from an earlier attempt makes the next one
report `DEVICE NOT FOUND`. Check `watch_connected` in RAM before suspecting
anything deeper.

---

## Reference: root cause of the original boot failure (2026-07-27)

Kept because the failure mode recurs after any `flash-sd`.

No bootloader on the chip → `UICR NRFFW[0]` unset → `fds_flash_end_addr()` fell
back to `CODESIZE × CODEPAGESIZE` = `0x100000` → FDS claimed `0xFE000`/`0xFF000`,
which are the **bootloader settings pages** → no valid page pair →
`fds_init()` returned `FDS_ERR_NO_PAGES` → `APP_ERROR_CHECK` at
`last_device.c:60` → fatal. `make flash-app` renewed the collision on every
flash by writing `settings.hex` to `0xFF000`.

Fix: `make flash-full`. **If the app dies at boot, check `UICR NRFFW[0]`
(`0x10001014`) first** — it must read `0x000F4000`. After any `make flash-sd` or
`pyocd flash --erase chip`, only `flash-full` restores it; `flash-app` will not.

---

## RETRACTED — do not act on these

Conclusions from earlier sessions that were wrong. Kept so they are not
re-litigated.

### ❌ "The nRF52840 is defective — LFCLK dead. Replace the board."

**Wrong.** Measured on the live chip:

| Register | Value | Meaning |
|---|---|---|
| `HFCLKSTAT` `0x4000040C` | `0x00010001` | SRC=Xtal, Running — 32 MHz crystal fine |
| `LFCLKSTAT` `0x40000418` | `0x00010001` | SRC=Xtal, Running — LFCLK fine |
| `LFCLKRUN` `0x40000414` | `0x00000001` | LFCLKSTART took |

The original test polled **`EVENTS_LFCLKSTARTED` (`0x40000104`)**, a
self-clearing one-shot latch cleared by the SDH clock handler — not a status bit.
The authoritative bit is `LFCLKSTAT.STATE`. The clock now runs on the **crystal**
(20 ppm, not the RC's 500 ppm), which matters here: LF accuracy sets the BLE
connection-event and ANT channel timing margin that three concurrent radios
depend on.

### ❌ "S340 v7.0.1 does not work on XIAO — switch to v6.1.1."

Not needed. v7.0.1 is flashed and both stacks enable. The v6.1.1 download can sit
unused.

### ❌ "`write_cmd_tx_queue_size = 8` will overflow `ram_start`."

Retired. The app links at `RAM ORIGIN = 0x20004000` and the SoftDevice enables
fine there.

### ❌ "USB enumeration is a cable/port problem." (2026-07-22)

Disproven by a known-good replacement cable — identical signature. See open
item 4.

### ⚠ Two measurement traps that produced false conclusions

- **Ticking RTCs do NOT mean the app is running.** RTC0/RTC1 are hardware
  counters and keep counting after the CPU faults. Confirm execution by halting
  and reading `pc`, or by a monotonic heartbeat.
- **A zero-valued state flag does NOT mean "healthy".** `s_dead = 0` and
  `s_fail_count = 0` were read as "the OLED is ACKing", but those are also the
  zero-init values — the code had never run. Check a flag only set on *success*
  (here `s_twi_ready`) before concluding anything.

---

## Notes (still valid)

- **`app_config.h` always beats `sdk_config.h`** — every `sdk_config` value is
  `#ifndef`-guarded and `app_config.h` is included first. A whole bring-up "fix"
  was once silently inert because of this.
- The ANT **license** key in `0b56e57..4fc7361` is Nordic's *published*
  evaluation key (verbatim at `nrf_sdm.h:191` in the public S340 download). Not a
  secret; no history rewrite warranted. Nothing genuinely secret was ever
  committed — only `.example` placeholders, both verified non-real.
- Publish via the clean-export flow (separate dir, single initial commit), which
  drops history anyway. ⚠ The ctrl-service UUID base is **not** a secret to be
  sanitized — see "Fixed this session".
- ANT network key is real, git-ignored, and confirmed by a real watch pairing to
  footpod #45694. The boot log looks *identical* with a zero key —
  `sd_ant_network_address_set()` does not reject one — so only a receiver can
  confirm it.

### SWD / flashing recipes

- **`make flash-full` is the only target that restores the bootloader and
  `UICR NRFFW[0]`.** `make flash-app` does not. `make flash-sd` chip-erases and
  destroys both.
- **⚠⚠ `make flash-app` IS BROKEN. Use `make flash-full`.** Reproduced three
  times on 2026-07-29 (twice deliberately), with both a growing and a shrinking
  image: the board parks in the bootloader at `pc = 0x000f8308`, `.bss` is never
  zeroed, `s_heartbeat_cnt` reads garbage. `flash-full` has never failed — used
  ~6 times that session, booted every time. **Root cause still not found**, but
  the search is now much narrower.

  Measured, so nobody re-derives it:
  - The settings page carries the app's validation CRC (`App Boot Validation
    Type: 1` = generated CRC), so an app flashed against a settings page
    describing a *different* app fails boot validation. That is the failure mode.
  - Passing app + settings to **one** `pyocd flash` **skipped the settings
    page**: pyocd printed `identical 4096 bytes (1 page)` and programmed only the
    app's 25 pages, leaving flash at app CRC `1afcbb43` while the freshly
    generated `settings.hex` held `505dcd7e`. `firmware/Makefile` now erases the
    settings sector explicitly (`SETTINGS_ADDR`), which does change skip →
    program (`identical 0 pages`).
  - **That is not sufficient.** After explicitly erasing and programming the
    settings page for a `TESTBOARD=0` image (size `0x00015EC4`, CRC `d4d9eb5d`),
    flash `0x000FF000` read back size `0x00018FE4` / CRC `505dcd7e` — the
    *previous* image's values — even though pyocd reported programming it.
    Something restores or overwrites the settings page after pyocd writes it.
  - The old "stale settings backup at `0xFE000`" theory stays dead: `nrfutil` is
    passed `--no-backup`, and `0xFE000` is the MBR params page. The
    shrinking-image hypothesis is also now unsupported — a *growing* image failed
    identically.

  **The one measurement nobody has taken:** read `0x000FF000` immediately after
  the `pyocd flash` of the settings page and *before* `pyocd reset`. That single
  read separates "pyocd never actually wrote it" from "the bootloader overwrote
  it on the next boot", and would probably finish this off. The bootloader is the
  obvious suspect for the latter.

  Until then: **`flash-full` after any app change**, and always confirm the app
  started (heartbeat check below) rather than assuming the flash worked. Note
  `flash-full` chip-erases, so it also wipes FDS and the saved last-device — the
  boot straight after a flash always reports "no saved device".
- **Confirm the app is running, not the bootloader.** Read `s_heartbeat_cnt`
  twice a few seconds apart: it must be a small, monotonically increasing value.
  Garbage means `.bss` was never zeroed. `pc` in `0xF4000+` is bootloader,
  `0x0`-`0x31000` is SoftDevice, the app is `0x31000+`. RAM survives a soft
  reset, so a stale RTT log and stale flags can look exactly like a healthy
  boot — always cross-check the heartbeat.
- **The `TESTBOARD` "links clean for both variants" check is a trap.** It leaves
  the `=0` artifact as the last build, and that is what gets flashed. Always
  rebuild `TESTBOARD=1` afterwards (this bit us on 2026-07-27).
- Recovery if SWD stops responding (no ACK): unplug the XIAO's USB-C, hold RESET,
  plug back in, release after 2 s. Forces a cold POR that restores the debug
  interface. Needed once this session.
- `pyocd flash --erase chip` does NOT erase the UICR.
- `pyocd erase --sector` treats addresses like `0xFF`/`0xFE` as unaligned and
  silently erases page `0x00000000`. Always use full addresses `0x000FE000` /
  `0x000FF000`.
- Read state without disturbing the target:
  `pyocd commander -t nrf52840 -O connect_mode=attach -c "read32 <addr>"`.
  `halt` / `go` to stop and restart the core (it is `go`, not `resume`).
- Plain reset does **not** enter DFU on this bootloader — it re-enumerates as the
  app. Entering DFU needs SWD (`make dfu-enter`) or the `DFU` ctrl command.

---

# Historical — session log from 2026-08-01

Pace-lag analysis of the first real SDM:TGT interval run, and the four fixes it
spawned. The headline numbers and expected post-fix values are in the top
section and in `docs/pace-lag-analysis.md`; this is the session's own account.

## The tool

`test/pace_lag_report.py` — a self-contained uv script (garmin-fit-sdk only).
It rebuilds what the bridge *should* have commanded straight from the .FIT's own
`workout_step_mesgs` + `lap_mesgs`, running a Python mirror of
`core/workout_ctrl.c decode_action()`, then scores the recorded trace against it.

The calculus is the headline metric. For a pure delay `τ` on a step of size
`|Δ|`, the area between the curves is exactly `|Δ|·τ`, so
`effective_lag = ∫|a−c|dt / Σ|Δᵢ|` is a lag in **seconds** that also absorbs
dropouts and level errors. Both signals are piecewise constant, so the
integrals are exact sums, not quadrature. The area splits into
**transient / dropout / steady** buckets that sum back to the total, so a
regression can be attributed. Two independent estimators cross-check it:
per-transition edge lag, and the global shift `τ*` that minimises the area.

`make pace-test` is the regression gate (self-test + baseline compare,
non-zero exit on regression). Verified that it fails when it should by feeding
it a doctored baseline.

## What the run showed

Every one of the 19 speed changes arrived — nothing lost, nothing stale.
Mean lag 2.26 s (median 2, max 5). The apparent "drift" in the first rounds is
phase between the watch's 1 Hz compute and the step boundary, not accumulating
error.

Caveat: the measured lag never reaches the belt. The ANT hop and the watch's
1 Hz recording are measurement artefacts — roughly 1 s of the 2.26 s. That's
why the ANT TX staging path was deliberately left alone: improving it would
improve a number the belt doesn't see.

The speed drops are **not** the belt slowing, and the file proves it two ways:
distance kept advancing at exactly the commanded rate through every hole, and
all five holes land at the same phase of the 17.000 s ANT page cycle (circular
concentration 0.984; scoring against a deliberately wrong 16 s period drops it
to 0.263, ruling out a method artefact). Cause: four background slots
back-to-back = a full second with no page 1, and page 2's status byte (per the
SDK's own `ant_sdm_page_2.h` header) said use state *inactive* — "this footpod
is not in use".

## Fixes (all built, none hardware-verified)

1. `DataFieldView.mc` — send from `onWorkoutStepComplete()`; fires at the step
   boundary instead of the next 1 Hz compute. Change-gated, so it can only help.
2. `CtrlBleDelegate.mc` — queue a frame that arrives mid-write and flush on
   completion (was a full-second penalty); also fixes a pre-existing gap where
   a reconnect left the view believing the bridge still held the last frame.
3. `core/ant_sdm_encode.c` — page 2 use state `0x00` → `0x01` (active).
4. `firmware/ant_sdm.c` — spread the background slots across a 64-slot cycle
   (slots 15/31/47/63) so no one-second window starves page 1.

Green: `make host-test`, `make firmware`, `monkeyc -l 2` (strict),
`--self-test`, `make pace-test`.

## Left for the user's call

- `test/23806153959_ACTIVITY.fit` stays **untracked** on purpose — it carries
  HR, timestamps and a device serial, and `make pace-test` depends on it.
- The predicted post-fix numbers in `docs/pace-lag-analysis.md` §4 are
  **predictions** until the same workout is re-run on hardware.
