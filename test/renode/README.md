# Renode Non-Radio Boot Smoke Test

## Purpose

This directory contains a bounded Renode smoke test that loads the firmware ELF
into an emulated nRF52840 and starts the CPU. It proves the firmware binary
loads, the vector table is valid, and the early boot path (clock, timers,
logging) begins executing.

**This is a smoke test, NOT a gate.** It does NOT prove the firmware works.

## Honest Limitations

### S340 SoftDevice Radio is NOT Emulable in Renode

Renode's nRF52840 BLE model targets **Zephyr's open-source BLE stack**, not
Nordic's proprietary **S340 SoftDevice** binary. The following are the specific
gaps that prevent S340 from running in Renode:

| Missing Feature | Why It Matters |
|---|---|
| **TEMP peripheral** | The S340 uses the on-die temperature sensor for radio calibration during SoftDevice initialization. Renode does not model `NRF_TEMP`. The S340 `sd_softdevice_enable` SVC call reads TEMP registers and faults if they return 0 / unmapped values. |
| **RSSI sampling / radio errata** | The S340 radio init sequences perform RSSI-based calibration and apply workarounds for nRF52840 errata 153 and 225. These require cycle-accurate radio peripheral state that Renode's simplified radio model does not provide. |
| **NVMC / UICR softdevice region** | The S340 expects to be pre-programmed at specific flash addresses (0x1000) and reads its configuration from UICR. Renode's flash model does not have the S340 binary loaded. |
| **SVC dispatch table** | The firmware calls SoftDevice APIs via `SVC` instructions. Without the S340 binary loaded, the SVC handler vectors are unmapped, leading to HardFault. |
| **Radio timeslot scheduler** | S340's internal timeslot manager for concurrent BLE+ANT relies on radio event timing (RTC0 compare, RADIO TX/RX interrupts) that Renode's abstracted radio model does not replicate at the register level. |

### What Renode CAN Verify

With the SoftDevice calls stubbed behind a HAL, Renode can verify:

1. The ELF loads at the correct flash origin (`APP_CODE_BASE` for S340).
2. The vector table (initial SP, reset handler) is valid and the CPU starts.
3. Non-radio peripheral drivers function: GPIO, I2C (SSD1306 OLED), timers,
   PWM (buzzer), RTT logging.
4. `core/` protocol logic runs (pure computation, no HW dependencies).

### What This Script Actually Does

The `.resc` script loads the platform, loads the ELF, starts the CPU, and lets
it run for 5 seconds. Without SoftDevice stubs, the firmware will reach
`softdevice_init()` (the `nrf_sdh_enable_request()` SVC call) and then:

- **If the S340 binary is not loaded:** the SVC handler is unmapped, causing a
  HardFault. This is expected and confirms the ELF loaded and the CPU reached
  the SoftDevice init call.
- **If SoftDevice stubs are in place:** the boot path continues through USB-CDC
  init and the idle loop, logging `alive N` heartbeat lines via RTT.

## Installing Renode

```sh
# macOS
brew install --cask renode

# Linux (via packages.renode.io)
# See: https://renode.io/#downloads

# Verify
renode --version
```

## Manual Invocation

From the repository root:

```sh
renode --console -e "include @test/renode/nrf52840_boot.resc; quit"
```

Or start an interactive Renode session:

```sh
renode --console
```

Then at the Renode monitor prompt:

```
(monitor) include @test/renode/nrf52840_boot.resc
(monitor) quit
```

## Adding SoftDevice Stubs for a Fuller Smoke Test

To exercise the full boot path (past `softdevice_init`), the firmware must
expose a compile-time `SOFTDEVICE_STUBBED` HAL that replaces every
`nrf_sdh_*` / `sd_*` call with a stub returning success (or the appropriate
error for "not enabled").

Example stub for `nrf_sdh_enable_request`:

```c
#if SOFTDEVICE_STUBBED
// In Renode, the S340 binary is not loaded; return success immediately.
uint32_t nrf_sdh_enable_request(void) { return NRF_SUCCESS; }
bool     nrf_sdh_is_enabled(void)        { return true; }
#endif
```

With `SOFTDEVICE_STUBBED=1` compiled in, the `.resc` script above would see
the heartbeat lines on the RTT analyzer.

## Status (2026-07-21)

- `renode` is NOT installed in the current environment.
- The `.resc` script and this README document the intended usage and honest
  limitations.
- This task is **non-gating** for the Milestone 1 exit gate. The real boot
  validation happens on hardware (M1 exit gate: firmware boots and logs over
  USB-CDC on the physical XIAO nRF52840).
