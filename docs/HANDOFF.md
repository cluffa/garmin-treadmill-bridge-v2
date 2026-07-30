# Handoff — 2026-07-29

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

Gates: `make host-test` 9/9; firmware links clean with `-Werror` for both
`TESTBOARD=0` and `TESTBOARD=1`.

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

### 1. Unbounded tight reconnect loop on `0x3E` (highest priority)

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

### 2. Advert names arrive empty — blocks the watch device picker

Every scan line this session read `central: found "" rssi -60 (iFit)`, yet
`I_TL` is known by connect time. `LIST` sent to the watch would therefore show
blank names, making the `garmin_ctrl_app` picker unusable. Likely the name is in
the **scan response** rather than the primary advert payload, and `adv_name()` in
`ble_central.c` only parses the latter. Fix before relying on the picker.

Not a blocker for the data field, which writes to `A6ED0004` and needs no names.

### 3. `A6ED0004` workout-frame writes are unlogged

`ble_ctrl_svc.c` `on_write()` calls `workout_ctrl_on_frame()` with no
`NRF_LOG`, while every sibling input logs (`rx "SCAN"`, `notifications on`). The
single most important product path — watch sets target → belt follows — leaves no
trace, which is why A3 had to be verified by reading RAM. Add a log line.

### 4. USB CDC never completes enumeration — firmware exonerated

Unchanged from 2026-07-27 and **not** a blocker: RTT over SWD is a strictly
better instrument for this work and is how the entire gate was run.

The device appears as `"Garmin Treadmill Bridge"` with `bNumConfigurations = 1`
but **zero `IOUSBHostInterface` children** and no `/dev/cu.usbmodem*`. The USB
state machine runs to completion (`USB-CDC initialized` → `USB power detected` →
`USB ready` → `USBD started`), so `app_usbd_init()`, `app_usbd_class_append()`,
`app_usbd_enable()` and `app_usbd_start()` all succeed. The **cable theory is
disproven** (known-good replacement, identical signature). What remains is the
configuration descriptor, a multi-packet EP0 IN transfer.

Untested suspects, cheapest first:
1. A different USB port / a different host machine. Only one Mac port tried.
2. The XIAO's USB-C connector or D+/D- routing — a marginal joint can pass
   low-speed EP0 setup and fail a longer multi-packet IN.
3. The CDC class descriptor set itself. **Capture the actual bus traffic**
   (USB analyser, or Wireshark + `XHC20` on macOS) and read the failing control
   transfer — the one measurement nobody has taken; it would settle this in
   minutes rather than another round of hypothesis-swapping.

Discriminator: the Pico probe enumerates fully on the same machine and port
family, so the host stack is fine; only the XIAO's link fails.

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

1. **Sideload the data field.** Connect the fenix 8 Solar 51mm over USB (mass
   storage) and copy:
   ```
   $OLD/garmin_data_field/out/app.prg  →  GARMIN/APPS/ on the watch
   ```
   `fenix8solar51mm` is already in the manifest's product list (alongside fr965,
   fr955, fr970). Optionally also `$OLD/garmin_ctrl_app/out/ctrl.prg` for the
   picker — but see open defect 2; names will be blank.
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
- **⚠ `make flash-app` left the board in the bootloader once — mechanism NOT
  explained.** After a `flash-app` rebuilding from a larger `-DDEBUG` image down
  to a smaller normal one, `pc` sat at `0xf8308` (bootloader) across four samples
  and `.bss` was never zeroed. A subsequent `flash-full` booted fine. A stale
  settings *backup* at `0xFE000` is **not** the cause (`Makefile:354` uses
  `--no-backup`; `0xFE000` is the MBR params page). Untested hypothesis:
  `--erase sector` only erases what it programs, so shrinking the image leaves
  the tail of the previous, larger app in flash. Until confirmed, **prefer
  `flash-full` after an app change** and always confirm the app actually started.
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
