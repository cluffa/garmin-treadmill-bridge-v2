# Handoff — 2026-07-27

## State

Branch `fix/code-review-2026-07-27`, 14 commits. `main` and `origin/main`
untouched (`origin/main` still at `e68a403` — nothing pushed).

**Uncommitted working-tree changes:** `firmware/testboard/ssd1306.c` (s_dead
backoff fix) and this file.

**The board boots and runs.** It was never defective. The boot failure had a
single root cause (missing bootloader → FDS could not allocate flash pages),
found and fixed this session. See "Root cause" below.

Verified on hardware after the fix: the heartbeat counter (`s_heartbeat_cnt`)
climbs monotonically at 1 Hz for 28 s+ with no fault, the log shows
`alive 63 … alive 77` running continuously, `last_device: fds ready`, ANT SDM
and BLE central both initialised, ctrl service advertising as `TMILL-CTRL`,
and the OLED is up (`s_twi_ready = 1`, `s_dead = 0`, `s_fail_count = 0`).

Gates: `make host-test` 9/9; firmware links clean with `-Werror` for both
`TESTBOARD=0` and `TESTBOARD=1`.

Hardware: XIAO nRF52840 + Pico CMSIS-DAP probe (GP2→SWCLK, GP3→SWDIO,
GND→GND). SWD is reliable.

**Only remaining blocker: USB CDC never completes enumeration** (see "Still
open").

---

## Root cause: no bootloader → FDS_ERR_NO_PAGES → fatal at boot

The app booted, enabled both stacks, started advertising, then hit a fatal
error and died. Everything downstream never ran.

The chain, each link measured:

1. `UICR NRFFW[0]` (`0x10001014`) read `0xFFFFFFFF` — **unset**. Flash at
   `0xF4000`/`0xF8000`/`0xFC000` was all `0xFFFFFFFF` — **no bootloader on the
   chip at all.**
2. With `BOOTLOADER_ADDRESS` unprogrammed, `fds_flash_end_addr()` falls back to
   `CODESIZE × CODEPAGESIZE` = `0x100000`, so FDS claims the top two pages:
   `0xFE000` and `0xFF000`.
3. Those are the **bootloader settings pages**. They held `0xdeadc0de` and
   `0x4f03edb3` — not erased, not valid FDS. Worse, `make flash-app` writes
   `settings.hex` to `0xFF000` on *every* flash, so the collision was renewed
   each time.
4. FDS found no valid swap/data page pair → `fds_init()` returned
   `FDS_ERR_NO_PAGES` (`0x860A` = 34314).
5. `firmware/last_device.c:60` is `APP_ERROR_CHECK(fds_init())` → fatal error →
   `NRF_BREAKPOINT_COND` → with the probe attached that BKPT becomes a
   HardFault, and the CPU parked in the `b .` loop of `HardFault_Handler`.
   **Without a probe attached it would instead be a silent reset loop**
   (`app_error_fault_handler` falls through to `NVIC_SystemReset()` when
   `DEBUG` is not defined).

This almost certainly came from a `make flash-sd`, which chip-erases and takes
the bootloader *and* UICR with it — exactly the ⚠ warning in `CLAUDE.md`.
Every `make flash-app` afterwards reflashed the app but never restored either.

### The fix

```sh
make flash-full TESTBOARD=1
```

Programs SoftDevice + app + bootloader + settings and sets `UICR NRFFW[0]`.
Verified afterwards: `UICR NRFFW[0] = 0x000F4000`, and the boot log runs clean
past the old failure point:

```
<info> app: ctrl_svc: initialized and advertising
<info> app: last_device: fds ready                    <-- was "Fatal error"
<info> app: central: initialized (scan module ready)
<info> app: ble_central: initialized
<info> app: ant_sdm init, dev_num=45694
```

`pc` now sits at `0x0002fe60` ("Device entered sleep") — the main loop's power
management, not a fault handler.

**If the app dies at boot again, check `UICR NRFFW[0]` first.** After any
`make flash-sd` or `pyocd flash --erase chip`, you must re-run
`make flash-full`; `make flash-app` alone will not restore it.

---

## How to debug this class of failure

This is the technique that found it — worth reusing.

**1. Read the log out of RAM over SWD.** No console needed. The RTT backend
buffer is a plain array; find it and dump it:

```sh
nm -S _build/nrf52840_xxaa.out | grep _acUpBuffer      # e.g. 2000552c
pyocd commander -t nrf52840 -O connect_mode=attach -c "read8 0x2000552c 640"
```

It is a 512-byte ring buffer, so it wraps — the tail of a long boot overwrites
the head.

**2. Get file:line on fatal errors by defining `DEBUG`.** Without it,
`app_error_fault_handler` logs only `"Fatal error"`. With it you get
`ERROR <code> [<name>] at <file>:<line>`, which is what identified
`last_device.c:60`. Temporarily add to `firmware/Makefile`:

```make
CFLAGS += -DDEBUG
```

(Removed again — the tree is back to the default. Note `DEBUG` also suppresses
the `NVIC_SystemReset()` on error, so the board parks instead of reset-looping.)

**3. Decode SDK error codes.** `34314` = `0x860A`;
`NRF_ERROR_FDS_ERR_BASE` is `0x8600` (`sdk_errors.h:96`), offset 10 in the
`fds.h:85` enum = `FDS_ERR_NO_PAGES`.

**4. Locate a fault.** `halt`, `reg pc`, then `addr2line`. Read `HFSR`
(`0xE000ED2C`) and `CFSR` (`0xE000ED28`): `HFSR=0x80000000` is DEBUGEVT (a
`BKPT` executed — an assert, not a bad pointer), and the stacked exception
frame at MSP gives the faulting PC and its caller.

**5. ⚠ Do NOT `halt` the core while the SoftDevice is running.** Halting
breaks the SD's radio/timeslot timing and trips an assert, so the board dies a
few seconds later *because you looked at it*. This cost real time this session:
a halt-every-4s poll made a perfectly healthy app appear to die at ~11 s. Plain
`read32`/`write8` in `connect_mode=attach` do **not** halt and are safe — poll
a counter instead. Use `halt` only on a board that has already faulted.

**6. The RTT buffer fills up and then silently drops.** It is 512 bytes and the
NRF_LOG backend uses non-blocking *skip* mode, so once `WrOff` reaches the end
with nothing draining it, **every later message is discarded** — including
fatal-error messages. That is why the boot log always appeared to stop at
`ant_sdm init`. The control block `_SEGGER_RTT` (find it with `nm`) has
`aUp[0]` at `+0x18`: `pBuffer +0x1C`, `SizeOfBuffer +0x20`, `WrOff +0x24`,
`RdOff +0x28`. Drain it by hand to make room for new output:

```sh
pyocd commander -t nrf52840 -O connect_mode=attach -c "write32 <RdOff_addr> <WrOff_value>"
```

---

## RETRACTED — do not act on these

Conclusions from earlier in this session that were wrong.

### ❌ "The nRF52840 is defective — LFCLK dead. Replace the board."

**Wrong. The board is fine and now runs.** Measured on the live chip:

| Register | Value | Meaning |
|---|---|---|
| `HFCLKSTAT` `0x4000040C` | `0x00010001` | SRC=Xtal, STATE=Running — 32 MHz crystal oscillates |
| `LFCLKSTAT` `0x40000418` | `0x00010000` | SRC=RC, STATE=Running — LFCLK fine |
| `LFCLKRUN`  `0x40000414` | `0x00000001` | LFCLKSTART triggered and took |

The original test polled **`EVENTS_LFCLKSTARTED` (`0x40000104`)**, which reads
`0` on this board while its LFCLK is demonstrably running. That register is a
**self-clearing one-shot latch** cleared by the SDH clock handler — not a
status bit. The authoritative bit is `LFCLKSTAT.STATE`.

The "32 MHz HFXO also non-functional" claim is disproven by `HFCLKSTAT.SRC=Xtal`
and by the board enumerating on USB at all — nRF52840 USBD cannot run without
HFXO.

### ❌ "S340 v7.0.1 does not work on XIAO — switch to v6.1.1."

Not needed. v7.0.1 is flashed and both stacks enable ("BLE stack enabled",
"ANT stack enabled"). The v6.1.1 download can sit unused.

### ❌ "`write_cmd_tx_queue_size = 8` will overflow `ram_start`."

Retired. The app links at `RAM ORIGIN = 0x20004000` and the SoftDevice enables
fine there. No `ram_start` change needed.

### ⚠ Two measurement traps that produced false conclusions

- **Ticking RTCs do NOT mean the app is running.** RTC0/RTC1 are hardware
  counters and keep counting after the CPU faults. Confirm execution by
  halting and reading `pc`.
- **A zero-valued state flag does NOT mean "healthy".** `s_dead = 0` and
  `s_fail_count = 0` were read as "the OLED is ACKing", but those are also the
  zero-init values — the code had never run. Check a flag that is only set on
  success (here `s_twi_ready`) before concluding anything.

---

## Fixed this session

### OLED was compiled out, not broken

The last build before this session was **`TESTBOARD=0`**, which strips
`testboard.c` and `ssd1306.c` from the link entirely (0 matching symbols in the
ELF; there are now 9). The Makefile default is `TESTBOARD ?= 1`, so this came
from an explicit `make firmware TESTBOARD=0` — almost certainly the "links
clean for both variants" check — which left the `=0` artifact as the last
build, and that is what got flashed.

Now confirmed working on hardware: `s_twi_ready = 1` (so `ssd1306_twi_start()`
actually ran and succeeded), `s_dead = 0`, `s_fail_count = 0` — the panel is
ACKing on I2C at address 0x3C.

### NRF_LOG re-entrancy into `app_usbd` (uncommitted, `firmware/app_config.h`)

`NRF_LOG_DEFERRED` was `0`, so every `NRF_LOG_*` dequeued **synchronously** into
the backends at the call site — and the CDC backend's `put()` ends in
`app_usbd_cdc_acm_write()`. `usbd_user_ev_handler()` logs from inside
`app_usbd`'s own event callbacks:

```c
case APP_USBD_EVT_POWER_DETECTED:
    NRF_LOG_INFO("USB power detected");   /* -> app_usbd_cdc_acm_write() */
    app_usbd_enable();                    /* ...before the stack is enabled */
case APP_USBD_EVT_POWER_READY:
    NRF_LOG_INFO("USB ready");            /* -> app_usbd_cdc_acm_write() */
    app_usbd_start();                     /* ...before the stack is started */
```

So the log backend re-entered `app_usbd` mid-dispatch, before
`app_usbd_enable()`/`app_usbd_start()` had run — and `cdc_tx_raw()` makes that
call inside `CRITICAL_REGION_ENTER()`, i.e. with interrupts masked, while EP0
enumeration traffic needs servicing. Introduced by `d3c1d60`.

Set `NRF_LOG_DEFERRED 1` (+ `NRF_LOG_BUFSIZE 2048`), which moves the backend
write out to `NRF_LOG_PROCESS()` in the main loop and breaks the re-entrancy.

**This did NOT fix USB enumeration** — see "Still open". It is a genuine
latent defect fixed on its own merits, and the visibility it bought is what
proved the USB stack reaches `USBD started`.

Also raised `SEGGER_RTT_CONFIG_BUFFER_SIZE_UP` 512 → 4096. At 512 the boot log
filled at `ant_sdm init` and the RTT backend's non-blocking *skip* mode
silently discarded everything after it — which is why the USB events and the
original fatal error were invisible for so long.

### `s_dead` latch in `ssd1306.c` (uncommitted)

`edb62f5` replaced `APP_ERROR_CHECK(nrf_drv_twi_tx(...))` with a fault counter,
but `s_dead` was set in two places and **never cleared anywhere**. Its commit
message claims "the counter reset by any success, so a transiently failing
panel can recover" — only `s_fail_count` reset; `s_dead` was permanent. A NACK
burst during the panel's own ~100 ms power-up would kill the display until
reboot.

Now a backoff rather than a latch: `ssd1306_show()` clears the fault and
re-runs the bring-up every `SSD1306_RETRY_FLUSHES` (50) flushes ≈ 10 s at the
5 Hz render tick. `ssd1306_init()` is split into `ssd1306_twi_start()`
(idempotent, tolerates `NRF_ERROR_INVALID_STATE`) and `ssd1306_panel_init()`
(the datasheet sequence), because a panel that lost power also lost its
configuration registers — re-sending the framebuffer alone would not revive it.
`ssd1306_text()` is no longer gated on `s_dead`, so the RAM framebuffer stays
valid and correct content goes out the instant the panel recovers.

**Verified on silicon** by fault injection over SWD:

```sh
nm -S _build/nrf52840_xxaa.out | grep -E ' s_dead| s_retry_skips'
pyocd commander -t nrf52840 -O connect_mode=attach -c "write8 <s_dead> 1"
# then poll <s_dead> and <s_retry_skips>
```

```
t+3s   s_dead=1  s_retry_skips=18    <- counting up at the 5 Hz render tick
t+6s   s_dead=1  s_retry_skips=35
t+9s   s_dead=0  s_retry_skips=0     <- crossed 50, re-ran bring-up, panel ACKed
t+12s..t+24s     s_dead=0, stays healthy
```

Before the fix the same test sat at `s_dead=1` for the full 21 s.

---

## Still open

- **USB CDC never completes enumeration — and the firmware is now exonerated.**
  The device appears as `"Garmin Treadmill Bridge"` with
  `bNumConfigurations = 1` but **zero `IOUSBHostInterface` children**, and no
  `/dev/cu.usbmodem*` of its own. The host gets the device and string
  descriptors; the configuration descriptor never lands.

  With the enlarged RTT buffer the whole boot is now visible, and the USB
  state machine **runs to completion**:

  ```
  <info> app: USB-CDC initialized
  <info> app: USB power detected     <- APP_USBD_EVT_POWER_DETECTED -> app_usbd_enable()
  <info> app: USB ready              <- APP_USBD_EVT_POWER_READY    -> app_usbd_start()
  <info> app: USBD started           <- APP_USBD_EVT_STARTED
  ```

  So `app_usbd_init()`, `app_usbd_class_append()` (CDC ACM),
  `app_usbd_power_events_enable()`, `app_usbd_enable()` and `app_usbd_start()`
  all succeeded. Two previously-leading suspects are therefore **dead**: the
  power-event handshake fires correctly, and the CDC class is appended. The
  main loop pumps `app_usbd_event_queue_process()` normally
  (heartbeat keeps counting).

  What remains is below the firmware: the configuration descriptor is a
  **multi-packet EP0 IN transfer**.

  **The cable theory is now DISPROVEN too.** A known-good replacement USB-C
  cable was fitted and the signature is unchanged — still
  `bNumConfigurations = 1`, still zero interface children. The 2026-07-22
  "physical: cable + USB port" diagnosis should not be trusted as-is.

  So both ends are excluded: the app-side stack reaches `USBD started`, and the
  cable is not at fault. Remaining suspects, none yet tested:
  1. **A different USB port / a different host machine.** Only one Mac port has
     ever been tried. Cheapest remaining test.
  2. **The XIAO's USB-C connector or D+/D- routing** — a marginal solder joint
     passes low-speed EP0 setup but fails a longer multi-packet IN.
  3. **The CDC class descriptor set itself.** `app_usbd_class_append()` returns
     success, but that does not prove the assembled configuration descriptor is
     well-formed. Capture the actual bus traffic (a USB analyser, or Wireshark
     + `XHC20` on macOS) and read the failing control transfer — that is the
     one measurement nobody has taken, and it would settle this in minutes
     instead of another round of hypothesis-swapping.

  A useful discriminator: the Pico probe enumerates fully on the same machine
  and port family, so the host stack is fine; only the XIAO's link fails.
- ~~**Real ANT+ network key.**~~ **DONE.** Copied from the old repo
  (`$OLD/boards/xiao-nrf52840/ant_network_key.h`) — identical format and
  symbol (`ANT_PLUS_NETWORK_KEY[8]`). Verified non-zero and still git-ignored
  (`.gitignore:3`). Flashed; boots clean with
  `ant_sdm: initialized and broadcasting`, `dev_num=45694`.
  Note the boot log looks *identical* with a zero key —
  `sd_ant_network_address_set()` does not reject one — so the only way to
  confirm the key is from a receiver. **Confirmed: the watch pairs and
  connects** to footpod #45694 (device type 124, Stride SDM). The ANT leg of
  M4.2 is proven end-to-end.
- ~~**Revisit `NRF_SDH_CLOCK_LF_SRC`.**~~ **DONE — the crystal works.**
  Reverted `9c1e7bb`'s RC fallback: `NRF_SDH_CLOCK_LF_SRC 1`,
  `NRFX_CLOCK_CONFIG_LF_SRC`/`CLOCK_CONFIG_LF_SRC 1`,
  `NRF_SDH_CLOCK_LF_ACCURACY 7` (20 ppm), and `RC_CTIV`/`RC_TEMP_CTIV` zeroed
  as XTAL requires. Confirmed by positive measurement, not inference:
  `LFCLKSTAT` (`0x40000418`) reads **`0x00010001` = SRC:Xtal, STATE:Running**.
  This is the definitive close on the "dead LFXO" claim.
  Matters for M4.2: LF accuracy sets BLE connection-event and ANT channel
  timing margin, and this device runs BLE peripheral + BLE central + ANT master
  concurrently — 20 ppm vs 500 ppm is the difference between comfortable and
  marginal in that window.
- **`APP_ERROR_CHECK(fds_init())` is still fatal-on-error.** `f01f79d` bounded
  the FDS *wait* so a slow init cannot hang the boot, but an FDS *error* still
  kills the device. `last_device.c` already has an `s_fds_unavailable` flag for
  exactly this; consider degrading gracefully instead, so a flash-layout
  problem costs you last-device memory rather than the whole product.
- **Test the `DFU` ctrl command** (`b4c3845`) end-to-end.

---

## Notes (still valid)

- **`app_config.h` always beats `sdk_config.h`** — every `sdk_config` value is
  `#ifndef`-guarded and `app_config.h` is included first. A whole bring-up
  "fix" was once silently inert because of this.
- The ANT **license** key in commits `0b56e57..4fc7361` is Nordic's *published*
  evaluation key (verbatim at `nrf_sdm.h:191` in the public S340 download). Not
  a secret; no history rewrite warranted. Nothing genuinely secret was ever
  committed — only `.example` placeholders, both verified non-real.
- Publish via the clean-export flow (separate dir, single initial commit),
  which drops history anyway.
- Still unfixed, deliberately: `BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE` is handled
  but does not requeue dropped iFit frames.
- Open GATT server (no pairing) is an **accepted risk**, recorded in the design
  spec.

### SWD / flashing recipes

- **`make flash-full` is the only target that restores the bootloader and
  `UICR NRFFW[0]`.** `make flash-app` does not. `make flash-sd` chip-erases and
  destroys both — that is what broke this board.
- **⚠ `make flash-app` left the board in the bootloader once this session —
  mechanism NOT explained.** After a `flash-app` (rebuilding from a larger
  `-DDEBUG` image down to a smaller normal one), `pc` sat at `0xf8308`
  (bootloader) across four samples and `.bss` was never zeroed — the app was
  never started. A subsequent `make flash-full` booted fine.
  A first theory — a stale settings *backup* at `0xFE000` — is **wrong**:
  `firmware/Makefile:354` generates settings with `--no-backup`, and `0xFE000`
  is the MBR params page (`docs/flashing.md:89`). `flash-app` does regenerate
  and reflash the settings page, so on paper it should be correct.
  Untested hypothesis worth checking: `--erase sector` only erases the sectors
  it programs, so shrinking the image leaves the tail of the previous, larger
  app in flash. Whether that can upset boot validation is unverified — it is
  one `make flash-app` away from being confirmed or ruled out.
  Until then, **prefer `make flash-full` after an app change**, and always
  confirm the app actually started (see the heartbeat check below) rather than
  assuming the flash succeeded.
- **Confirm the app is actually running, not the bootloader.** Read
  `s_heartbeat_cnt` (`nm -S`) twice a few seconds apart: it must be a small
  monotonically increasing value. Garbage there means `.bss` was never zeroed,
  i.e. the app's startup never ran. A `pc` in the `0xF4000+` range is
  bootloader code; `0x0`–`0x31000` is SoftDevice; the app is `0x31000+`.
  Note RAM survives a soft reset, so a stale RTT log and stale flags can look
  exactly like a healthy boot — always cross-check with the heartbeat.
- Recovery if SWD stops responding (no ACK): unplug the XIAO's USB-C, hold
  RESET, plug back in, release after 2 s. Forces a cold POR that restores the
  debug interface.
- `pyocd flash --erase chip` does NOT erase the UICR.
- `pyocd erase --sector` treats addresses like `0xFF`/`0xFE` as unaligned and
  silently erases page `0x00000000`. Always use full addresses `0x000FE000` /
  `0x000FF000`.
- Read state without disturbing the target:
  `pyocd commander -t nrf52840 -O connect_mode=attach -c "read32 <addr>"`.
  `halt` / `go` to stop and restart the core (it is `go`, not `resume`).
- Plain reset does **not** enter DFU on this bootloader — it re-enumerates as
  the app. Entering DFU needs SWD (`make dfu-enter`) or the new `DFU` ctrl
  command.
