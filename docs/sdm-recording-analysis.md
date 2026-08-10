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

---

# The follow-up run — 2026-08-10

Source data: `23928343294_ACTIVITY.fit` — 33 min wall / 28:37 timer, 4850 m,
single-step workout ("Base", 9:25/mi, target band 2.694–3.027 m/s), recorded on
the first firmware carrying the clock fix above. Untracked, like the others.

## 8. The clock fix worked

The prediction in §4 was that fixing the frozen page-1 clock would restore the
lap accumulators. It did:

| | 2026-08-05 (pre-fix) | 2026-08-10 (post-fix) |
|---|---|---|
| lap `total_distance` | **0.0 on all 8 laps** | **4850.39** |
| lap `avg_speed` | **absent on all 8** | `enhanced_avg_speed` 2.825 |

That is the whole of the defect described in §4, closed.

Worth recording because it was the one soft spot in the diagnosis: page 1
carries *instantaneous speed* in bytes 4/5, so a receiver does not need the
clock to know the current speed, and it was not obvious that a frozen clock
alone would take out the lap summaries. It did. The lap accumulators integrate
against the sensor's clock even though the speed field does not.

## 9. Cadence is still broken, exactly as predicted

`avg_running_cadence` 253 strides/min, `max_running_cadence` **0**, `avg_power`
570 W, `avg_step_length` 716.9 mm. §7 said to expect this and why: the stride
counter in page 1 byte 6 never moves, so Δstrides is zero however good the
clock is. Page 22 remains the open lever, and its prerequisite header is
present on this machine at
`$SDK_ROOT/components/ant/ant_profiles/ant_sdm/pages/ant_sdm_page_22.h`.

## 10. Two pace drops, two unrelated causes

The run showed three speed excursions. One was deliberate — a stop at the 2 mi
mark (timer 18:53, distance 3225 m = 2.004 mi). The other two were not, and
they turned out to have nothing to do with each other.

### 10a. timer 25:00 — nothing is wrong

A single record carrying no `enhanced_speed` at all. Distance runs straight
through it at 2.855 m/s:

```
29:21  2.855  4225.13
29:22  None   4227.99      <- renders as a dip; belt never changed
29:23  2.855  4230.84
```

13 of 1719 records (0.76%) are like this. It is RF loss, not the TX slot
pattern — the interleave in `firmware/ant_sdm.c` guarantees at least three
page 1s inside any one-second window, so the pattern cannot produce a whole
second with no speed. **No action.** A receiver that renders a missing sample
as a dip is drawing the gap honestly.

### 10b. timer 23:34 — the treadmill link dropped and the footpod cried stop

```
27:55  2.855  3988.86     normal
27:56  0.0    3988.86  <- speed zero, distance FROZEN
27:57  0.0    3988.86
27:58  0.0    3988.86
27:59  2.678  3991.54     resumes, with no catch-up jump
28:03  2.855  4002.43     back to target
```

Two fingerprints say this is a data event, not a belt event:

- **The distance froze.** A belt decelerating from 2.855 m/s still covers
  ground, and it cannot reach zero inside one sample. The odometer stopping
  dead means the source stopped reporting.
- **No catch-up on resume.** Distance picks up at +2.68 m/s from where it
  froze, rather than jumping to account for the ~8.5 m the belt really
  travelled — the mark of an odometer that was reset and re-baselined, not one
  that kept counting.

The cause was in the disconnect handler, which zeroed the whole treadmill
struct under the comment *"belt stops when the link drops"*. That premise is
false: a dropped BLE link is a comms event, and the treadmill keeps running at
its set speed. The ANT footpod broadcast that zeroed struct directly, so a 3 s
dropout told the watch the runner had stopped dead while they were still going
6.4 mph, and reset the broadcast odometer on top of it.

The same file proves the premise false on its own: the deliberate stop at 18:53
went through the *connected* path, arriving as a reported speed 0 over a link
that stayed up. Real belt stops never take the disconnect path.

**Fixed** in `core/sdm_broadcast.c` (host-tested; there is no practical
hardware test for a 3-second dropout). The footpod now keeps its own broadcast
speed and odometer, the same way it already keeps its own clock:

- last known speed is **held** for `SDM_LINK_GRACE_S` (5 s) across a link drop,
  with the odometer advancing to match, then falls to an honest 0;
- the odometer is **monotonic**, accumulated from source deltas, and rejects
  implausible deltas in *both* directions. Both directions matters:
  `core/ftms_parse.c` memsets its output and fills distance only when Treadmill
  Data flag bit 2 is present, so an FTMS treadmill that omits the field on
  alternate notifications flaps real → 0 → real, and rejecting only negative
  deltas would bank the rising edge as a kilometre-scale jump.

A stop the treadmill actually reports still passes through untouched, which is
the case that must not regress.

Residual, accepted: `central_link` reaches `LINK_UP` slightly before the first
notification repopulates speed, so one or two 4 Hz ticks of the post-memset
zero can still reach the air on reconnect. Sub-second, and below the watch's
1 Hz record rate — in this .FIT the first post-gap sample was already at 2.678.

**Verification is the next recorded run**: the 10b signature (speed 0.000 with
a frozen odometer) should be extinct. 10a may well recur and is not a fault.
