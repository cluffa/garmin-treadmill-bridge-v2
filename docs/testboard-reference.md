# Testboard reference (TESTBOARD=1)

When `TESTBOARD=1`, the firmware compiles in the expansion-board test aid:
SSD1306 OLED (128×64 I²C), onboard RGB LED, passive buzzer, and user button.
All of this lives behind `#if TESTBOARD` — production builds (`TESTBOARD=0`)
compile it out to no-ops.

## RGB LED (onboard XIAO nRF52840)

Pins: Red P0.26, Green P0.30, Blue P0.06 — all active-low.

| Colour            | Treadmill link | Watch       | Fault | Notes                      |
|-------------------|:--------------:|:-----------:|:-----:|----------------------------|
| Off               | `LINK_DOWN`    | —           | no    | Idle, nothing happening    |
| Blue blink (fast) | `LINK_SCANNING`| —           | no    | BLE scan in progress       |
| Yellow            | `LINK_CONNECTING`| —         | no    | Red + green = yellow       |
| Green             | `LINK_UP`      | no          | no    | Treadmill linked, no watch |
| Cyan              | `LINK_UP`      | **yes**     | no    | Green + blue = cyan        |
| Red               | *any*          | *any*       | **yes**| Fault overrides everything |

The LED is updated at ~5 Hz by the render tick, so scanning blink is
approximate (the tick drives 5 on/off cycles per second rather than a
hardware PWM blink).

## OLED display (8 rows × 21 columns)

| Row | Label          | Source field                  | Notes                                    |
|-----|----------------|-------------------------------|------------------------------------------|
| 0   | `Lnk:`         | `central_link`                | `D`=down, `S`=scanning, `C`=connecting, `U`=up |
| 0   | `W:`           | `watch_connected`             | `+`=connected, `-`=not connected         |
| 0   | `A:`           | `ant_broadcasting`            | `+`=broadcasting, `-`=not broadcasting; `T`=broadcasting **commanded target speed** (SDM target-broadcast debug mode) |
| 1   | `Belt:`        | `treadmill.speed_mps`         | Actual belt speed, formatted as km/h      |
| 2   | `Targ:`        | `resolved_target_mps`         | Resolved target speed from workout_ctrl  |
| 3   | `Act:`         | button cycle position         | Current short-press action (see below)   |
| 4   | `Fault:`       | `last_fault_code`             | 0 = no fault                             |
| 5   | `[btn]`        | —                             | Hint: `[btn] cycle  long:RST`            |
| 6–7 | (spare)        | —                             | Unused                                   |

Speeds are shown in km/h with one decimal place (e.g. `  8.0`), right-aligned
in a 6-character field. Integer arithmetic is used to avoid pulling in
`_printf_float` from newlib.

The OLED framebuffer is composed inside the ~5 Hz render timer callback, but
the blocking I²C flush (`ssd1306_show()`, ~23 ms at 400 kHz) is deferred to
`testboard_process()` called from the main loop so it never stalls BLE/ANT
event dispatch inside an IRQ.

## Button (expansion board, XIAO D1 / P0.03)

| Press            | Duration    | Action                                                    |
|------------------|-------------|-----------------------------------------------------------|
| Short            | < 800 ms    | Executes the current action, then cycles to the next one  |
| Long             | ≥ 800 ms    | `NVIC_SystemReset()` — full device reboot                 |

**Action cycle** (short-press rotates through these):

| # | OLED label    | What it does                                                     |
|---|---------------|------------------------------------------------------------------|
| 0 | `SCAN`        | Start BLE scan for treadmills (`testboard_action_scan`)          |
| 1 | `CONN-NEXT`   | Connect to next available treadmill (`testboard_action_connect_next`) |
| 2 | `INJECT 8.0`  | Inject a synthetic 15-byte workout frame (8.0 km/h target) into `workout_ctrl` |
| 3 | `STOP`        | Stop/disconnect (`testboard_action_stop`)                       |
| 4 | `SDM:TGT`     | Toggle footpod between actual belt speed and the commanded target speed (`sdm_broadcast_target`) — see below |

SCAN, CONN-NEXT, and STOP are `__attribute__((weak))` stubs that the radio
layer (M3) overrides. INJECT works standalone (no radios needed) — it builds
a valid `A6ED0004` workout frame and feeds it directly to the platform-agnostic
`workout_ctrl_on_frame()`, which resolves the target and calls
`machine_set_speed()`.

## SDM target-broadcast mode (debug aid)

`SDM:TGT` toggles `app_state()->sdm_broadcast_target`. When on, the ANT
footpod broadcasts the **commanded target** speed (`resolved_target_mps`,
what `workout_ctrl`/ctrl-dispatch last told the treadmill) instead of the
actual belt speed, and distance is integrated from that target so the trace
is self-consistent. The OLED row 0 `A:` indicator turns `T` and the action
label reads `SDM:TGT` / `SDM:ACT`.

**Purpose:** score belt accuracy from the watch's .fit file. Run the same
workout twice on the watch — once with the footpod in normal mode (the .fit
speed trace is the *actual* belt response) and once in target mode (the trace
is exactly what the bridge commanded) — and diff the two traces. The mode
resets to actual at boot; toggling off re-seeds time and distance from the
actual belt state on the next broadcast.

**Works with no treadmill connected**, which is the other reason to use it —
drive the bridge from the watch alone and watch what it *would* have commanded.
Two things make that work: the target latches regardless of the belt link
(`machine_set_speed()` sets `resolved_target_mps` before its connection check),
and the broadcast time and distance are integrated from the app_timer RTC
rather than from `treadmill.elapsed_s`, which only advances while a treadmill
is actually sending FTMS data.

## Buzzer chirps (passive buzzer, PWM on expansion board A3/D3)

The buzzer chirps on every state transition (not continuously). Pitch and
duration distinguish the event:

| Event                        | Frequency | Duration | Notes                      |
|------------------------------|:---------:|:--------:|----------------------------|
| Link state change            | 1500 Hz   | 40 ms    | Any `central_link` transition |
| Watch connect/disconnect     | 1800 Hz   | 40 ms    | `watch_connected` toggle   |
| ANT broadcast start/stop     | 2200 Hz   | 40 ms    | `ant_broadcasting` toggle  |
| Fault code change            |  600 Hz   | 120 ms   | Lower, longer — attention  |
| Button short-press: SCAN     |  800 Hz   | 60 ms    |                            |
| Button short-press: CONN-NEXT| 1200 Hz   | 60 ms    |                            |
| Button short-press: INJECT   | 2000 Hz   | 80 ms    |                            |
| Button short-press: STOP     |  500 Hz   | 100 ms   |                            |
| Button short-press: SDM:TGT  | 2400 Hz   |  80 ms   | Target broadcast ON        |
| Button short-press: SDM:TGT  |  700 Hz   |  80 ms   | Target broadcast OFF       |
| Button long-press (reset)    |  400 Hz   | 200 ms   | Then 250 ms delay before NVIC reset |

## Rendering tick

The render timer fires at ~5 Hz (200 ms interval). On each tick it:
1. Chirps the buzzer on any state transitions
2. Updates the RGB LED via `hw_led_status()`
3. Composes all 8 OLED rows into the SSD1306 framebuffer
4. Sets a flag for the main loop to flush the framebuffer to I²C

The deferred flush is critical: running `ssd1306_show()` inside the timer
callback would block I²C at IRQ priority 6 (same as `SD_EVT_IRQn`), stalling
BLE/ANT event dispatch ~12% of the time at 5 Hz.
