# What the footpod does to a recorded activity — 2026-08-05

Source data: `23856353712_ACTIVITY.fit` — a 48-minute interval treadmill run
(8 laps, avg HR 155, max 174) recorded on the fenix 8 with the bridge's ANT+
footpod paired and the CIQ data field driving the belt. Untracked, like the
other activity .FITs (see CLAUDE.md).

The activity's summary numbers were visibly wrong in Garmin Connect. This is
what the file actually says, what caused it, and what is still open.

---

## 1. The data field is not involved

`watch/garmin_data_field/manifest.xml` requests only `BluetoothLowEnergy`.
There is no `FitContributor` permission and no `createField` call anywhere in
`watch/`, so the data field contributes no FIT fields and cannot affect the
recording. It writes to `A6ED0004` and nothing else.

The `CONNECT IQ` section that appears in Connect's stats page for this activity
(`Elevation Gain(+)`, `Elevation Gain(-)`, `Total Distance`, and the
`inclineRunn` developer field on every record) comes from a *different* CIQ
field on the same data screen.

## 2. The watch sources cadence from the footpod, and the footpod says zero

The decisive number: **`record.cadence` is 0 in 2758 of 2890 records (95.4%)**.

The 132 non-zero records are one contiguous window, t+308 s to t+439 s, where
cadence reads 71–81 strides/min — i.e. 142–162 spm, correct. Everywhere else
the watch records a flat zero.

So the `0xFF` / `0xF` invalid-cadence encoding shipped in `d5782ac` **does not
work**. This settles open item 5 in `docs/HANDOFF.md`, negatively: the watch
did not fall back to wrist cadence. It is still taking cadence from us, and we
are still effectively telling it zero — the pre-`d5782ac` behaviour, reached by
a different route.

Where the zero comes from is almost certainly **page 1 byte 6, the stride
count** (`core/ant_sdm_encode.c`), which is hard-coded `0x00` and never
increments. Page 1 goes out in 60 of every 64 slots; page 2 in 2. A receiver
differentiating a stride counter that never moves gets zero strides, and no
`0xFF` in the page-2 cadence field changes that.

The watch's own step counter is unaffected and correct — `total_strides` is
3870 over 2886.6 s of timer time, or 80.4 strides/min (161 spm), and every lap's
`total_strides` is right. The wrist knows the cadence. It just isn't what lands
in `record.cadence`.

## 3. Everything downstream of cadence is garbage

| Field | Recorded | Should be |
|---|---|---|
| `avg_running_cadence` | 246 strides/min (Connect doubles it → **494 spm**) | ~80 strides/min |
| `max_running_cadence` | **0** | ~85 |
| `avg_power` / `max_power` | **613 W** / 989 W | ~230 W |
| `normalized_power` | 638 W | — |
| `avg_step_length` | 784 mm | 8814.56 m ÷ 7740 steps = **1139 mm** |
| `avg_vertical_oscillation` | 29.4 mm | ~85 mm |
| `avg_vertical_ratio` | 3.75% | ~7.5% |

Garmin's running-power and running-dynamics models take cadence as an input, so
a zero cadence propagates into all of them. Note the summary figures are not
even self-consistent with each other: 3.054 m/s ÷ 784 mm implies 234 spm, while
`avg_running_cadence` implies 494 spm. Both are accumulator garbage rather than
a coherent wrong answer.

## 4. Every lap has zero distance and no speed at all

```
lap 0: total_timer_time 600.0  total_distance 0.0  avg_speed None  total_strides 775
lap 1: total_timer_time 480.0  total_distance 0.0  avg_speed None  total_strides 670
...   (all 8 laps identical in this respect)
```

`record.distance` climbs normally to 8814.56 m and the session carries that
total, but **`total_distance` is 0.0 on all 8 laps and `avg_speed` /
`enhanced_avg_speed` are absent on all 8**.

This is the footpod's frozen clock. Until 2026-08-05 page 1's time field came
from `treadmill.elapsed_s`, and:

- `firmware/ble_central.c` `on_hvx_ifit()` assigned it a literal `0` on every
  single notification — iFit frames carry no elapsed time;
- `core/ftms_parse.c` fills it only when the treadmill sets Treadmill Data flag
  bit 10, and `memset`s the struct to zero otherwise.

So on an iFit treadmill the footpod broadcast **time = 0 forever** while its
distance field advanced normally: a sensor whose odometer moves against a
stopped clock. Any receiver-side `Δdistance / Δtime` is a division by zero.

Page 1's *fractional* time byte was independently hard-zero, which mattered for
the same reason: the footpod broadcasts at ~4 Hz, so even a correct
whole-second clock repeats its timestamp across three of every four pages.

Both are fixed in `firmware/ant_sdm.c` / `core/ant_sdm_encode.c` as of
2026-08-05 — see §6.

## 5. The Firstbeat metrics are near-zero and this is not explained

```
total_training_effect            0.3
total_anaerobic_training_effect  0.0
Exercise Load                    5
Stamina                          100% -> 99%
Body Battery net impact          -1
```

Against an HR record showing 22:44 in zone 4 and 9:19 in zone 5, these are not
low — they are absent. A load of 5 is roughly what one minute at that intensity
produces, not 48. The HR stream itself is intact (`avg_heart_rate` 155,
`max_heart_rate` 174, `min_heart_rate` 63, 2890 records with no gaps).

No cause has been established for this. It is plausibly downstream of §4 — the
lap speed/distance accumulators being dead means Firstbeat's running model has
no external load to work with — but that is a hypothesis, not a finding.
Re-record after the §6 fix and re-check before investigating further.

## 6. What changed (2026-08-05)

The footpod now keeps its own clock.

- `firmware/ant_sdm.c` integrates elapsed time from the app_timer RTC on every
  TX event, in **both** broadcast modes and with no treadmill connected. This
  logic already existed but was fenced inside the `SDM:TGT` branch. It is
  wrapped at 256 s, which is where the wire field rolls anyway, so float32
  resolution stays constant over a long session.
- `core/ant_sdm_encode.c` fills page 1's fractional time byte (1/256 s) instead
  of hard-zeroing it, so consecutive pages at 4 Hz carry distinct timestamps.
- `core/model.h` `treadmill_state_t.elapsed_s` is a `float` for that reason.
- `firmware/ble_central.c` no longer assigns `elapsed_s = 0` on every iFit
  notification. Nothing reads it for the broadcast now, but the assignment was
  actively misleading.

Asserted by `test_ant_sdm_encode`, including the property that four consecutive
quarter-second pages all differ in the time field.

## 7. Still open: cadence

**The time fix does not fix cadence.** Δstrides is zero regardless of how good
the clock is, because the stride counter never moves. Expect §2 and §3 to look
the same on the next recording.

The bridge cannot measure cadence — it has no stride sensor and must not invent
one. The goal is the opposite: get the watch to stop treating us as a cadence
source so it falls back to its own wrist data, which §2 shows is correct and
available.

The remaining lever is **SDM capabilities page 22 (0x16)**, which exists to
declare which fields a sensor actually supports. We do not broadcast it at all
today, and adding it means a new slot in the 64-slot TX cycle in
`firmware/ant_sdm.c`.

⚠ **Its bit layout has not been verified.** It could not be confirmed from the
public Nordic docs, and shipping a guessed capabilities bitfield is worse than
shipping nothing — the same byte carries the speed and distance validity bits,
and flagging either of those unsupported by mistake would break the product's
core function silently. Before implementing, read one of:

- `$SDK_ROOT/components/ant/ant_profiles/ant_sdm/pages/ant_sdm_page_22.h`
  (present on the build machine at `/Users/alex/nRF5_SDK_17.1.0_ddde560`);
- the ANT+ Stride Based Speed and Distance Monitor device profile from
  thisisant.com.

If page 22 turns out not to carry a cadence-validity bit, the fallback worth
testing is dropping page 1 in favour of page 2 as the main page — page 2 has no
stride-count field at all — but that also drops the distance field, so it needs
its own recorded run to evaluate.
