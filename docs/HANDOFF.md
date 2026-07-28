# Handoff — 2026-07-27

## State

Branch `fix/code-review-2026-07-27`, 13 commits, tree clean. `main` and
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

---

## Session 2026-07-27 (continued) — hardware bring-up attempt

### Completed

- **Pico CMSIS-DAP probe attached** (GP2→SWCLK, GP3→SWDIO, GND→GND).
- **DFU ctrl command** added (`b4c3845`): `DFU` over USB-CDC or BLE writes
  `0xB1` to GPREGRET and resets into the bootloader. Works over SWD;
  in-app use needs a booting firmware.
- **RC clock config** committed (`9c1e7bb`): `NRF_SDH_CLOCK_LF_SRC=0` in
  `firmware/app_config.h`. Should be reverted to XTAL (`=1`) for a good
  board.
- **S340 v6.1.1** downloaded to
  `~/workspace/nrf52/ANT_s340_nrf52840_6.1.1/ANT_s340_nrf52840_6.1.1.hex`.
  The Seeed forum reports v6.1.1 works on XIAO where v7.0.1 does not:
  https://forum.seeedstudio.com/t/assistance-needed-loading-softdevice-s340-onto-seeed-xiao-ble-sense-nrf52840-board/275100/4
- `make host-test` — all 9/9 pass. Firmware builds clean with `-Werror` for
  both TESTBOARD=0 and =1.
- 13 commits on `fix/code-review-2026-07-27`; 2 new since the original
  handoff.

### BLOCKED: defective nRF52840 — LFCLK subsystem dead

The on-die 32 kHz RC oscillator does not oscillate. Caught the CPU at the
MBR entry point (`0xA80`, before any SoftDevice code) and tested both RC
(`LFCLKSRC=0`) and XTAL (`LFCLKSRC=1`): `EVENTS_LFCLKSTARTED` never fires
in either mode. Without LFCLK the SoftDevice (both v6.1.1 and v7.0.1) spins
forever waiting for the clock and never forwards to the app.

External crystals (32 kHz LFXO and 32 MHz HFXO) are also non-functional on
this board, but the RC failure is definitive — the RC needs zero external
components and should always work on a functional nRF52840.

This is a known XIAO issue:
- XIAO crystal unreliable: https://forum.seeedstudio.com/t/xiao-ble-unstable-when-32khz-clock-source-is-crystal/291575/3
- nRF52840 RC latch-up errata: https://devzone.nordicsemi.com/f/nordic-q-a/106619/when-frozen-then-reset-nrf52840-rc-osc-rtc-doesn-t-tick-and-lfclksrc-etc-doesn-t-init-correctly/460258

### Recovery notes

- The board can be put into a state where SWD stops responding (no ACK).
  Recovery: unplug XIAO USB-C, hold RESET button, plug back in, release
  after 2 seconds. This forces a cold POR that restores the debug interface.
- `pyocd flash --erase chip` does NOT erase the UICR (confirmed: NRFFW
  survives).
- `pyocd erase --sector` interprets addresses like `0xFF` or `0xFE` as
  unaligned and silently erases page 0x00000000. Always use full addresses
  `0x000FE000` / `0x000FF000`.
- The bootloader persistently restores its settings pages (backup at
  0xFE000 mirrors 0xFF000). Flashing one without erasing the other causes
  the bootloader to revert. Flash both or use `--erase chip`.
- `make flash-full` programs the bootloader, which sets NRFFW[0] in UICR.
  `make flash-sd` + `make flash-app` skips the bootloader (SD → app
  directly). Neither works with a dead LFCLK.

### Still needed

- **Replace the XIAO board.** The LFCLK defect is silicon-level.
- Revert `NRF_SDH_CLOCK_LF_SRC` to `1` (XTAL) once on a working board.
- Update the Makefile to use S340 v6.1.1 instead of v7.0.1 (or test v7.0.1
  on the new board first).
- Read the actual `ram_start` value from RTT on a booting board;
  `write_cmd_tx_queue_size = 8` may push it above `0x20004000`.
- Real ANT+ network key in `firmware/ant_network_key.h`.
- Test the DFU ctrl command end-to-end once the app boots.
