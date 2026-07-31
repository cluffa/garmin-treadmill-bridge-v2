# Mock bridge on macOS — design

**Date:** 2026-07-30
**Status:** approved, ready for planning

## Problem

Iterating on `watch/garmin_data_field` currently requires the physical nRF52840
bridge in the loop: build the `.prg`, sideload over USB mass storage, start a run
activity, and read the bridge's USB-CDC console to see whether the frame arrived
and what it did. Firmware changes need a flash on top of that. The feedback loop
is long enough that watch-side work stalls.

A macOS-hosted mock of the bridge's BLE peripheral role removes the hardware
bridge from that loop. The watch stays real; the bridge becomes a laptop.

## Goal

Verify that the data field's existing telemetry — the 15-byte workout frame
written to `A6ED0004` — reaches a bridge cleanly and produces the belt action it
should, with the Mac as an observation rig. Once that loop exists, use it to
explore what *additional* data is worth putting on the wire. Extending the frame
is explicitly a later decision, not part of this spec.

## Non-goals

- No new wire-format fields. This spec observes the format as it is today.
- No ctrl-grammar emulation (`A6ED0002` / `A6ED0003`, `SCAN` / `CONNECT` /
  `LIST`, `D`/`E`/`S` frames). `garmin_ctrl_app` is not exercised here.
- No JSONL capture, no replay, no TUI dashboard.
- No Connect IQ simulator path. The watch is real, over the air.

## Approach

Python + [`bless`](https://github.com/kevincar/bless) for the BLE peripheral
role, mirroring the existing `test/mock/mock_treadmill.py`, with the belt-action
prediction coming from the **actual** `core/workout_ctrl.c` loaded through
ctypes.

Rejected alternatives:

- **Reimplement the decode policy in Python.** Cheaper to write, but the value of
  the predicted action is that it is what the real bridge would do. A second
  implementation of `decode_action()` is precisely the silent divergence that the
  UUID placeholder incident already cost this project (`watch/README.md`).
- **Native Swift CLI over `CBPeripheralManager`.** Closest to CoreBluetooth and
  avoids the `bless` dependency, but introduces a Swift toolchain to a C/Python
  repo with no existing pattern. Reconsider only if `bless` cannot advertise the
  128-bit UUID (risk 1).

## Components

| File | Responsibility |
|---|---|
| `test/mock/workout_probe.c` | `machine_*` stubs that record the last command, plus the exported probe API. Compiles against `core/workout_ctrl.c`. |
| `test/mock/Makefile` | Builds `libworkout_probe.dylib` with the host `cc`. |
| `test/mock/mock_bridge.py` | `bless` peripheral, ctypes binding, decoding, logging. |

### `workout_probe.c` interface

Scalars only across the ABI — no structs, so there is no layout to keep in sync:

```c
void  probe_reset(void);                              /* workout_ctrl_reset() */
int   probe_feed(const uint8_t *buf, uint16_t len);   /* -> action code */
int   probe_tick(void);                               /* -> action code */
float probe_last_speed(void);                         /* km/h of last ACT_SPEED */
```

Action codes: `0` none, `1` speed, `2` stop. `probe_feed` and `probe_tick` return
what the stubbed `machine_set_speed` / `machine_stop` observed during that call,
so a deduplicated frame correctly reports `none`.

The `machine_*` stub set is the one already proven in
`test/host/test_workout_ctrl.c`: `machine_start_scan`, `machine_get_devices`,
`machine_connect`, `machine_connected`, `machine_connected_device`,
`machine_set_incline`, `machine_set_speed`, `machine_stop`.

This file lives under `test/mock/`, not `core/`, so the `core/` purity invariant
is untouched.

### `mock_bridge.py`

Advertises local name `TMILL-CTRL` with service UUID
`A6ED0001-D344-460A-8075-B9E8EC90D71B`. GATT table:

| Char | Properties | Behaviour |
|---|---|---|
| `A6ED0002` | write | Logged raw, otherwise ignored. Present so discovery matches the firmware's table. |
| `A6ED0003` | notify | Never notifies. Present for the same reason. |
| `A6ED0004` | **write with response** | The one that matters. Decoded, fed to the probe, logged. |

A 1 Hz asyncio task calls `probe_tick()` so the firmware's slow keepalive
re-assert appears in the log exactly as it would on hardware.

## Data flow

1. Mock advertises; the data field's `CtrlBleDelegate` scans and matches on the
   128-bit UUID **in the advertisement** (`ScanResult.getServiceUuids()`).
2. `pairDevice` → connect → service discovery.
3. On change, `DataFieldView._maybeSend` writes 15 bytes to `A6ED0004`.
4. The mock's write handler logs decoded fields, calls `probe_feed`, logs the
   action.
5. The 1 Hz task calls `probe_tick`; a non-`none` return is logged as a
   keepalive re-assert.

## Log format

One line per event, `HH:MM:SS.mmm` prefix:

```
21:04:12.310  ADV   TMILL-CTRL  A6ED0001-D344-460A-8075-B9E8EC90D71B
21:04:31.882  LINK  central connected
21:04:33.104  WKT   #1  len=15 ver=1  timer=3(ON) flags=0x01 intensity=0(active)
                        tgt=0(SPEED) lo=2222 hi=2500 mm/s (8.0–9.0 km/h) dur=5(TIME) 300 rep=0
                        -> ACT_SPEED 8.5 km/h
21:04:34.101  idle  (no write — field sends on change only)
21:05:03.900  KEEP  -> re-assert 8.5 km/h
21:05:41.220  WKT   #2  timer=2(PAUSED) flags=0x00 -> ACT_STOP
21:06:02.115  WKT   #3  len=12  MALFORMED (expected 15) — dropped
```

The `idle` heartbeat, throttled to roughly every 10 s while connected, is
deliberate. The field writes only on change, so a silent log is *correct*
behaviour — and without the heartbeat it is indistinguishable from a dead link.
That ambiguity has already cost this project debugging time twice
(`watch/README.md`, "a free run will not move the belt").

Sentinel values are rendered by name: `0xFF` for intensity / targetType /
durationType prints as `255(unset)`, and the free-run signature
(`timer=3 flags=0x00 tgt=255`) is annotated inline as `FREE RUN — belt held` so
it is not mistaken for a fault.

Malformed frames are detected and logged by the mock itself.
`workout_ctrl_on_frame()` drops bad length/version silently, so the mock checks
length and version before feeding the probe.

## Error handling

- **Wrong length or version** → logged `MALFORMED`, not fed to the probe.
- **Dylib missing** → the script exits at startup with the `make` command to
  build it, rather than falling back to a Python reimplementation.
- **Advertising failure** → surfaced immediately with the CoreBluetooth error;
  no silent retry loop.
- **Central disconnect** → logged, `probe_reset()` called so a stale keepalive
  does not survive into the next session, and advertising resumes.

## Testing

`workout_probe.c` gets no new unit tests — `test/host/test_workout_ctrl.c`
already covers the C it exposes. What is new is glue, and the honest test for it
is hardware bring-up. Two things wire into the existing gate:

- `test/mock/mock_bridge.py` is added to `CONSUMERS` in
  `test/check_uuid_contract.py`, so it cannot drift from the firmware's UUID base.
- A `make mock-bridge` target builds the dylib and runs the script.

Bring-up acceptance, on a real watch:

1. Watch finds and connects to `TMILL-CTRL` (link line appears).
2. A free run logs `timer=3 flags=0x00` and `ACT_NONE` — the belt-held case.
3. A structured workout with a speed target logs `ACT_SPEED` at the midpoint of
   `lo`/`hi`.
4. Pause logs `ACT_STOP`; resume re-commands.
5. Holding a steady step for >30 s produces a `KEEP` re-assert.

## Risks

Ordered by likelihood of biting.

1. **macOS advertising a 128-bit service UUID.** The CIQ delegate filters on the
   *advertisement*, not the GATT table. A 16-byte UUID plus header plus the name
   `TMILL-CTRL` is 30 of the 31 available bytes — it fits, but barely, and
   CoreBluetooth may relocate or drop the name. This is the load-bearing
   assumption of the whole design. **The first task in the implementation plan is
   a minimal spike that advertises and does nothing else, verified with an
   independent scanner, before any decoding code is written.** If it fails,
   fall back to approach C (Swift over `CBPeripheralManager`).
2. **Write-with-response.** `CtrlBleDelegate.writeWorkoutFrame` uses
   `WRITE_TYPE_DEFAULT`. `A6ED0004` must be declared writable *with* response, or
   writes fail with a status the field only prints to its own console.
3. **The physical bridge must be powered off** during a session. Otherwise the
   field pairs with whichever device it finds first and the session debugs the
   wrong peer.
4. **Pairing behaviour.** `pairDevice` may trigger a macOS pairing prompt or
   bonding semantics the nRF52 handles differently. Unknown until the spike runs.

## Dependencies

`bless` (already used by `test/mock/mock_treadmill.py`, declared inline via the
`uv run --script` header). Host `cc` for the dylib. No SDK, no ARM toolchain.
