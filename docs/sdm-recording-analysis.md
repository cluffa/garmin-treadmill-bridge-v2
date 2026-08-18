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

> **Resolved 2026-08-11.** The layout was read off that SDK header and page 22
> now broadcasts — see §11. It *does* carry a cadence-validity bit.

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

That lever is now pulled — §11.

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

---

# Capabilities page 22 — 2026-08-11

## 11. The footpod now declares cadence unsupported

§7 named page 22 as the last lever and blocked on one thing: the capabilities
bitfield's layout was unverified, and the same byte carries the speed and
distance validity bits, so a wrong guess would flag the product's core function
unsupported with no visible error. **That block is cleared.** The layout is
spelled out in the SDK profile, which is on the build machine:

`$SDK_ROOT/components/ant/ant_profiles/ant_sdm/pages/ant_sdm_page_22.h`

```c
bool time_is_valid     : 1;   /* bit 0 */
bool distance_is_valid : 1;   /* bit 1 */
bool speed_is_valid    : 1;   /* bit 2 */
bool latency_is_valid  : 1;   /* bit 3 */
bool cadency_is_valid  : 1;   /* bit 4 */
bool calorie_is_valid  : 1;   /* bit 5 */
```

A set bit means supported. The matching `ant_sdm_page_22.c` gives the rest of
the wire layout: one capabilities byte followed by six reserved bytes that its
encoder `memset`s to `UINT8_MAX`. So the page on the air is

```
0x16 0x07 0xFF 0xFF 0xFF 0xFF 0xFF 0xFF
      ^^^^ time | distance | speed supported;
           cadence, latency, calories NOT
```

Latency is left unsupported deliberately: we never compute one — page 1 byte 7
is a constant `0x00` — so claiming it would be a second small lie in a page
whose entire purpose is to stop telling the first one. Page 1 and page 2 are
otherwise untouched. In particular the stride counter stays `0x00`: a rolling
counter has no invalid encoding, which is exactly why no edit to the *data*
pages can fix this and page 22 is the only place in the profile that can.

### The TX slot

Page 22 takes **slot 7** of the 64-slot cycle in `firmware/ant_sdm.c`,
converting one page-1 slot. `CYCLE_LEN` stays 64, so pages 80/81 still recur
every 64 messages (inside the profile's 65-message requirement) and
`test/pace_lag_report.py`, which parses `CYCLE_LEN` out of that file, is
unaffected.

Slot 7 is not arbitrary. Every background slot is ≡ 3 (mod 4), and any four
consecutive slots — one second of air, one watch record — contain exactly one
slot at that residue. So at most one background page can land in any
one-second window and at least three page 1s always do, across the cycle wrap
included. That is the invariant §10a depends on; a background page at any other
residue would put two in some window and reintroduce the speed holes. Verified
by simulating the cycle from the actual `#define`s: 59 page 1s per cycle,
minimum 3 per one-second window, page 22 first on air 1.75 s after broadcast
start and every 16 s after.

### ⚠ It is broadcast unsolicited

The SDK's own SDM **master** never puts page 22 in its periodic rotation — its
`next_page_number_get()` emits it only from the request controller, i.e. when
the display asks with common page 70. We do not implement that; the ANT channel
is TX-only.

Broadcasting anyway is the right first move for two reasons. The SDK's
**display** side decodes page 22 whenever it arrives — a plain `case
ANT_SDM_PAGE_22` in the decode switch, with no request-matching — so an
unsolicited page is processed. And a request handler only helps if the watch
actually asks; a watch that queries capabilities at pairing time asked long ago,
and the periodic broadcast is the only path that reaches an already-paired one.

**If the next run still shows zero cadence, answering page 70 is the next
lever** — and it will be cleanly attributable, because this change will have
been ruled out.

### How to verify

Not verifiable on this machine: it needs a flashed board and a recorded run
(USB-DFU, since SWD is down — `docs/flashing.md` §8b).

**Re-pair the footpod before the verification run** — remove it from the watch's
sensor list and add it back. If the watch cached capabilities at pairing time,
when we advertised none, it may never re-read them, and a run recorded on a
stale pairing cannot distinguish "page 22 doesn't work" from "the watch never
looked at it".

Then check the .FIT for the §2/§3 signature:

- `record.cadence` should stop being flat zero and should track the wrist —
  ~80 strides/min at 161 spm, the value §2 showed the watch already has;
- `max_running_cadence` should stop being **0** and `avg_running_cadence`
  should land near 80, not 246–253;
- `avg_power` should fall from ~570–613 W to something near 230 W, and
  `avg_step_length` should rise toward the ~1139 mm the distance and step count
  imply.

Speed and distance must be unchanged — lap `total_distance` and
`enhanced_avg_speed` populated as in §8. Those are the fields the capabilities
byte could have broken, so they are the regression to check first.

## 12. Page 22 does not work either — 2026-08-11

Source data: `test/23943114012_ACTIVITY.fit` — a 44-minute 'Threshold' workout
(10:00 warmup @ 10.30 km/h, 20:00 @ 12.80, 10:00 cooldown @ 10.30), started
19:22 local, with a manual 4-minute pause at 19:52.

The §11 preconditions were both met. The footpod was removed from the watch's
sensor list and re-added **after** the flash, so the watch re-read capabilities
with page 22 on air. And the board itself was asked, not a flash log:

```
{"cmd":"status","connected":true,"name":"I_TL","build":"0811-1727 110d0a2","tx_drops":0}
```

`110d0a2` descends from the page 22 commit `03fa74b`, so the image that
broadcast during this run declared cadence unsupported. **The verdict is a
clean negative: page 22 changes nothing.**

### 12.1 The signature is unchanged

| | 2026-08-10 (no page 22) | 2026-08-11 (page 22) | should be |
|---|---|---|---|
| `record.cadence` flat zero | 1695/1719 (98.6%) | **2365/2406 (98.3%)** | ~80 |
| `avg_running_cadence` | 253 | **252** | ~80 |
| `max_running_cadence` | **0** | **0** | ~85 |
| `avg_power` | 570 W | 650 W | ~230 W |
| `avg_step_length` | 716.9 mm | 807.6 mm | ~1100 mm |

Not a partial improvement — the same defect, to within run-to-run noise.

### 12.2 The watch *does* have a wrist fallback, and a slow belt triggers it

This is the new evidence, and it is stronger than the negative result. In both
runs the nonzero-cadence records form **exactly one window** (t=2049–2089 here,
41 records; t=1403–1428 in the control, 24 records with a two-record dropout at
1406), and both windows open on a footpod speed collapse to the 0.886 m/s idle
floor:

```
2026-08-11 (page 22 on air)             2026-08-10 (control)
   t     spd    cad    pow                 t     spd    cad    pow
2049   0.886     48   None              1403   0.886     60   None   <- idle floor
2050   0.886     47     93              1404   0.886     60     15
2052   0.886     47     91              1405   0.886     60    117
2053   2.855     50    252   <- belt    1406   0.886      0     32
2058   2.855     65    237      back    1410   0.886     70    123
2061   2.855     76    373      up      1411   2.855     70    378   <- belt back up
2067   2.855     81    371              1418   2.855     80    377
2089   2.855     81    374              1428   2.855     81    376
2090   2.855      0    374   <- snaps   1429   2.855      0    581   <- snaps back
2091   2.855      0    579      back    1430   2.855      0    581
```

Two things to read off that, and the second is the important one.

**The wrist path is intact and its whole downstream is correct.** Inside the
windows, cadence settles at 81 strides/min (162 spm), power at ~373 W, and
`step_length` at ~1057 mm — all three within a few percent of what §3 predicted
from the distance and step count, against 650 W / 808 mm for the run as a whole.
The watch is not missing this data. It has it, and it computes correctly from it.

**The footpod stream was healthy the whole time, and the watch went back to it
anyway.** This is where the earlier framing of these windows as "the stream was
absent" was wrong: `enhanced_speed` is present and quantised at 2.855 m/s and
`distance` advances 2.855 m per record straight through, so for ~37 s (~18 s in
the control) the watch recorded **footpod speed and wrist cadence
simultaneously** — the two sources are selected independently. Then at t=2090 it
reverts to footpod cadence in a single record, zero again, power stepping
374 → 579 W. **Page 22 was on air throughout, every 16 s, declaring cadence
unsupported, and the watch chose us for cadence anyway.** That is the negative
result observed directly rather than inferred from the summary fields.

What triggers the fallback is a footpod speed collapse while the runner is still
moving, not the page 22 declaration — 2026-08-11's was the post-pause belt
spin-up, 2026-08-10's was the §10b link blip. It persists ~18–37 s past the
return to normal speed and then ends. No mechanism is claimed for that duration;
the 48 → 81 ramp at the start of each window is the watch's own cadence average
filling, not switch latency.

Two hypotheses survive, and they are now the whole question:

1. **The watch reads capabilities only by asking, and we never answer.** The
   re-pair happened with page 22 already on air, so a plain pairing-time cache
   does not explain this — but a watch that learns capabilities by *requesting*
   them with common page 70 and discards unsolicited page 22s does. §11 flagged
   exactly this asymmetry: the SDK's own master emits page 22 only from the
   request controller, and we have no request handler at all.
2. **The watch ignores page 22 for cadence-source selection entirely** and keys
   off the data pages alone, in which case no capabilities work can fix it.

Answering page 70 discriminates between them and is the next lever, exactly as
§11 said it would be — the difference now is that it is cleanly attributable,
because the unsolicited-broadcast path has been ruled out on hardware. It is
implemented in §13, and it comes with a counter that answers the question
without needing a run at all.

### 12.3 Speed and distance survived — the regression check passes

The first thing to check was whether the capabilities byte broke the fields it
shares a byte with. It did not. All four laps carry both accumulators:

```
lap 0  warmup    total_distance 1701.34  enhanced_avg_speed 2.836
lap 1  interval  total_distance 4251.50  enhanced_avg_speed 3.543
lap 2  cooldown  total_distance 1706.63  enhanced_avg_speed 2.844
lap 3  (3.975 s) total_distance    8.57  enhanced_avg_speed 2.156
session          total_distance 7668.04  enhanced_avg_speed 3.190
```

§8's fix is still in force with page 22 on air. Declaring time, distance and
speed supported (`0x07`) is correct and does no harm.

### 12.4 Belt tracking, and a scorer limitation this run exposed

Only two commanded transitions, so this is a weak lag sample, but both are
clean: edge lag **5 s** on 10.30→12.80 and **2 s** on 12.80→10.30, transient
area 4.88 m. Steady-state tracking is essentially exact — the recorded trace
sits +0.006 m/s off the 10.30 km/h command and +0.007 m/s off the 12.80 one.

⚠ **Ignore `pace_lag_report.py`'s headline on this file.** It reports
`effective_lag=540.628 s`, which is an artifact, not a regression: the scorer is
not pause-aware. The 247 s manual pause leaves a hole in the record stream while
the reference command `c(t)` runs straight through it, and the whole hole lands
in the *steady state* bucket — 2.86 m/s × 247 s = 706 m, against the 725.97 m
the report attributes there. The transient bucket (4.88 m) is unaffected and is
the number to read. Making the scorer skip `timer stop_all` → `timer start`
spans is a known todo; until then, score unpaused runs only.

The 30 flagged dropouts are 28 single missing records (RF loss, §10a — phase
concentration 0.281, not clustered on the page schedule) plus two genuine belt
events: the 6 s start ramp, and 4 s at 0.886 km/h at t=2049 where the belt was
still spinning up after the pause.

## 13. The footpod now answers page 70 requests — 2026-08-11

§12 left two explanations and named answering common page 70 as the lever that
separates them. This is that lever, plus a counter that settles the question
*before* the next run instead of after it.

`core/ant_request.c` (pure, `test/host/test_ant_request.c`) decodes a received
page 70 and decides which page goes on which TX slot; `firmware/ant_sdm.c` does
the radio half. Layout and accept/reject rules are taken from the SDK's own
sensor-side implementation, which is on the build machine:

```
$SDK_ROOT/components/ant/ant_profiles/ant_common/pages/ant_common_page_70.c
$SDK_ROOT/components/ant/ant_profiles/ant_common/ant_request_controller/
```

### 13.1 The counter is the experiment

`STATUS` now reports three fields:

```
{"cmd":"status",…,"tx_drops":0,"p70":0,"p70_rej":0,"p70_pg":0}
```

`p70` counts page 70s accepted, `p70_rej` those understood but not actionable,
`p70_pg` the page number the last accepted request asked for. Cumulative per
boot — `ant_sdm_start()` is called once from `main()`, so a treadmill reconnect
does not reset them.

**This is why they exist.** §12's two hypotheses are indistinguishable in a
.FIT, and one of them means no capabilities work can ever fix cadence. They are
trivially distinguishable here:

| after DFU, re-pair the footpod with the console attached | reading |
|---|---|
| `p70` increments | the watch **does** ask. Hypothesis 1 was right, we are now answering, and the next recorded run is decisive. |
| `p70` stays 0 through a re-pair *and* a run start | the watch **never** asks. Hypothesis 2 gains hard, page 22 cannot be delivered by any route, and cadence is not fixable from the capabilities side. |
| `p70_rej` non-zero with `p70` 0 | it asks for a page we do not build — read `p70_pg` and decide whether to add it. |

A zero counter is therefore a *result*, not a broken feature. Read it before
concluding anything about the run.

### 13.2 What it sends, and the invariant it bends

A request is acted on only if the command type is `0x01` (data page request),
the transmission-response byte is not `0x00`, and the requested page is one the
footpod actually builds — 1, 2, 22, 80 or 81. Anything else increments
`p70_rej` and is otherwise ignored, which is what the profile asks for: better
no answer than a wrong one. Bit 7 of the response byte selects acknowledged
messages, and the exact value `0x80` means "until acknowledged".

The first response goes out on the **very next** TX slot. A request satisfied
16 s later would be worth no more than the unsolicited rotation §12 already
disproved.

⚠ **A response displaces the rotation page for its slot**, which touches the
interleave invariant §10a depends on — at least three of any four consecutive
slots must be page 1, or the watch records a second with no speed. Two things
bound it:

- responses are spread **one per four slots**, so a one-second window holds at
  most one response plus at most one rotation background page. The guarantee
  degrades from three page 1s to two, never to zero.
- both modes are **capped** — 16 explicit transmissions, 8 until-acknowledged.
  The SDK caps neither, because the SDK has no interleave invariant to protect;
  this is a deliberate divergence.

The rotation *resumes* rather than skipping ahead, so no background page is ever
dropped — the pattern just shifts a slot later.

### 13.3 The event-pump change is the risky part

`ant_evt_handler()` used to pump the broadcast from `EVENT_TX` alone. An
acknowledged response completes as `EVENT_TRANSFER_TX_COMPLETED` or `_FAILED`
**instead of** `EVENT_TX`, so that handler would have stopped feeding the channel
the moment it answered its first acknowledged request, leaving the radio
retransmitting one stale page for the rest of the run. It now pumps on all
three, exactly as the SDK's `ant_sdm_sens_evt_handler()` does. Each slot yields
exactly one of the three, so this is still one pump per slot and the footpod
clock's RTC differencing is unaffected.

### 13.4 How to verify

`make host-test` and `make firmware` pass, and the map file confirms the three
`ctrl_sdm_p70_*` symbols resolve to `ant_sdm.c.o` rather than core's weak zeros,
so `STATUS` is reading the real counters.

**Flashed 2026-08-11 over USB-DFU** (§8b, no probe). The board's own readback:

```
{"cmd":"status","connected":true,"name":"I_TL","build":"0811-2159 4b5988c","tx_drops":0,"p70":0,"p70_rej":0,"p70_pg":0}
```

Two independent confirmations in one line — the stamp names the page-70 commit's
descendant, and the `p70` fields are *present at all*, which the previous image
could not do. The treadmill link came back up by itself after the reboot, so the
central is unharmed.

### 13.5 The watch asks for page 22, by name, at pairing — 2026-08-11

The footpod was removed from the watch's sensor list and re-added with the
console attached. The board:

```
{"cmd":"status",…,"build":"0811-2159 4b5988c","tx_drops":0,"p70":2,"p70_rej":0,"p70_pg":22}
```

**Hypothesis 1 is confirmed and hypothesis 2 is dead.** Two page 70 requests,
both actionable (`p70_rej` 0 — nothing malformed, nothing we could not build),
and the page requested was **22**: capabilities, by number.

So the watch does read capabilities. It reads them by *asking, at pairing time*,
and until this image we never answered. That is the complete explanation for §12:
broadcasting page 22 unsolicited for 44 minutes changed nothing because the watch
was not listening for it unsolicited — it had already asked, once, at pairing, and
got silence. The SDK's SDM master emits page 22 only from its request controller
(§11); the watch is the matching half of that contract and we were the only party
not honouring it.

Two smaller things this settles in passing:

- the whole receive path works on first contact with real hardware — request
  decoded, accepted, page 22 encoded and put on the next TX slot;
- `p70_rej` 0 means the watch asks only for pages we build, so the supported-page
  list (1/2/22/80/81) needs no additions.

**What is still unproven is whether answering fixes the recording.** The watch
now has our capabilities byte; whether it acts on it — falls back to wrist
cadence and stops recording our zero — is a separate question that only a run
answers. The remaining step:

1. Record a run on this pairing and check the §12.1 table:
   `avg_running_cadence` near 80 with `max_running_cadence` non-zero, `avg_power`
   near 230 W, `avg_step_length` near 1100 mm — and speed/distance unchanged, per
   §12.3. The wrist values to expect are the ones §12.2 measured directly during
   the fallback windows: 81 spm, ~373 W, ~1057 mm.
2. If cadence is *still* zero, the capabilities route is exhausted — the watch
   asked, we answered, and it ignored the answer. That would be a genuinely new
   finding and not a variation on §11 or §12.

⚠ **Do not re-pair again before that run.** The requests come at pairing, so the
answered pairing is the one now in place; re-pairing while investigating anything
else discards the state the experiment depends on.

---

# 14. The signature predates every fix — 2026-08-18

Asked directly: *was cadence already broken before page 22?* Yes — by ten days
and four recordings. Measured from the .FITs themselves rather than from the
changelog, with `tools/cadence_history.py`:

| recording | ANT+ sensor the watch saw | avg | max | records with cadence 0 |
|---|---|---|---|---|
| 2026-06-04 `23133263148` (outdoor run) | **none** | 75 | 90 | 112 / 3509 |
| 2026-08-01 `23806153959` | development/SDM (us) | — | — | 309 / 309 |
| 2026-08-03 `23842067586` | development/SDM | 0 | 0 | 304 / 304 |
| 2026-08-04 `23842755992` | development/SDM | 234 | 0 | 323 / 325 |
| 2026-08-04 `23842851505` | development/SDM | 0 | 0 | 302 / 302 |
| 2026-08-10 `23928343294` | development/SDM | 253 | 0 | 1695 / 1719 |
| 2026-08-11 `23943114012` | development/SDM | 252 | 0 | 2365 / 2406 |
| 2026-08-18 `24029215957` | **north_pole_engineering/SDM** | 75 | 83 | 54 / 3613 |
| 2026-08-18 fenix 6X | development/SDM | 253 | 0 | 276 / 276 |

Against `git log firmware/ant_sdm.c`:

- `d5782ac` **2026-07-31** — cadence marked invalid `0xFF`/`0xF` instead of a
  valid `0x00`. Every broken run in the table is *after* this, so the invalid
  encoding was already in place for all of them (§7 predicted this would fix it;
  it did not).
- `03fa74b` **2026-08-11** — capabilities page 22. Four broken runs precede it
  (Aug 1, 3, 4, 4, 10). Immediately before it: avg 253. Immediately after:
  avg 252. Today, with page 22 and page 70 both live: avg 253.
- `21e41f8` **2026-08-11** — page 70 responses. Same: no movement.

So neither §11 nor §13 changed the outcome, and neither could have *caused* it.
The single variable that moves the number is **which SDM sensor the watch is
paired to**: ours produces 253/max 0 in every run since the footpod first went
on air, and a real one (the NPE Runn, same belt, same hour) produces 75/83.

That is consistent with — and independently confirms — the control in
`docs/ant-logging.md`: the watch's recorded cadence tracked the Runn's
*broadcast page 2 cadence field* to 95.4 % of samples. The watch reads that
field. It reads ours too. Ours has never contained anything usable, from the
first run onward, and no page we have added since changes what is in it.

The one control the table is missing is a *treadmill* run with no footpod
paired at all; 2026-06-04 is an outdoor run, so it shows only that the wrist
sensor itself is fine. §12.2's in-run fallback windows — 81 spm measured while
the watch was ignoring us — cover that gap from the other direction.
