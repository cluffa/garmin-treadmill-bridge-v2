# Handoff — 2026-07-27

## State

Branch `fix/code-review-2026-07-27`, 11 commits, tree clean. `main` and
`origin/main` untouched (`origin/main` is still at `e68a403` — nothing pushed).

A three-part code review found 6 Critical + ~15 Important issues; all are fixed
and committed. **Nothing has been flashed** — every fix is verified only by host
tests, host ASan/UBSan, and a clean `-Werror` build. None has run on silicon.

Gates that pass: `make host-test` (9/9); firmware links clean for TESTBOARD=0
and =1.

## BLOCKED: cannot flash

The board is plugged in and enumerates as `"Garmin Treadmill Bridge"`, but has
**zero `IOUSBHostInterface` children** — device + string descriptors are served,
then nothing at configuration level. No `/dev/cu.usbmodem*` exists.

Established this session:
- Reset **works** (USB `sessionID` changes) but re-enumerates as the **app**,
  not the bootloader — so **plain reset does NOT enter DFU** on this bootloader.
- No SWD probe attached (`pyocd list` → none).
- Therefore every route is blocked: no console, no DFU node, no way to set
  GPREGRET.

A CDC config descriptor needs a multi-packet EP0 IN transfer. That exact
failure was diagnosed as **physical (cable + USB port)** on 2026-07-22 — see
the `nrf52-hw-bringup` memory.

### Do these in order

1. **Swap the USB-C cable.** Historical culprit for this exact signature.
   Check with `ls /dev/cu.usbmodem*`. If a node appears, flash over USB:
   `make dfu && make flash-dfu SERIAL=/dev/cu.usbmodemXXXX`
   (note: `dfu-enter` needs SWD, so this only works if the app is invalid —
   otherwise you still need step 2).
2. **Otherwise attach the Pico CMSIS-DAP probe** (GP2→SWCLK, GP3→SWDIO,
   GND→GND; no 3V3 if self-powered) and `make flash-app`.

### While the probe is attached, do all three

- Flash the branch.
- **Read the required `ram_start`.** `write_cmd_tx_queue_size = 8` was added
  (`main.c`, `sd_ble_cfg_set(BLE_CONN_CFG_GATTC, ...)`). It raises SoftDevice
  RAM demand. If it exceeds the linker's `0x20004000`, `nrf_sdh_ble_enable()`
  fails → `APP_ERROR_CHECK` → boot loop. It logs the required value; apply it to
  `firmware/xiao_nrf52840_s340.ld` if needed. **This is the most likely
  first-boot failure.**
- **Add a `DFU` ctrl command** so the probe is never needed again for routine
  updates: write `0xB1` to GPREGRET `0x4000051C`, then `NVIC_SystemReset()`.
  ~15 lines in `core/ctrl_dispatch.c` + the firmware side. The constants already
  exist in `firmware/Makefile` (`GPREGRET_ADDR`, `GPREGRET_DFU`). This closes a
  real gap: nothing in the app can currently reboot itself into the bootloader.

## Also blocks the hardware gate

`firmware/ant_network_key.h` is **eight zero bytes** — the placeholder. A zero
key is not the ANT+ network, so the SDM footpod will not pair with anything.
The ANT leg of the M4.2 concurrency gate will fail looking exactly like radio
contention. Get the real key from thisisant.com first.

## Notes

- **`app_config.h` always beats `sdk_config.h`** (every sdk_config value is
  `#ifndef`-guarded and app_config is included first). A whole bring-up
  "fix" was silently inert because of this. The XIAO **does** have an LFXO;
  the earlier "no LF crystal / use RC" conclusion was wrong and is reverted.
- The ANT **license** key in commits `0b56e57..4fc7361` is Nordic's *published*
  evaluation key (verbatim at `nrf_sdm.h:191` in the public S340 download). Not
  a secret; no history rewrite warranted. Nothing genuinely secret was ever
  committed — only `.example` placeholders, both verified non-real.
- Publish via the clean-export flow (separate dir, single initial commit), which
  drops history anyway.
- Still unfixed, deliberately: `BLE_GATTC_EVT_WRITE_CMD_TX_COMPLETE` is handled
  but does not requeue dropped iFit frames.
- Open GATT server (no pairing) is an **accepted risk**, recorded in the design
  spec.
