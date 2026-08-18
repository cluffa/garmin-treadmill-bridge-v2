# Logging another ANT+ sensor, to compare against our own footpod

The bridge transmits a synthesized ANT+ SDM footpod (`firmware/ant_sdm.c`,
channel 0). Everything this repo knows about whether that broadcast is *right*
has come from watch recordings after the fact — a .FIT is the only feedback
loop, it takes a whole run to produce, and it shows what the watch decided
rather than what any sensor sent.

`firmware/ant_log.c` opens a second ANT channel — a slave (channel 1) — that
locks onto some **other** ANT+ sensor in the room and dumps every page it
receives to the USB console. A real treadmill sensor and ours are then logged by
one board, off one `app_timer`, with no clock reconciliation.

## Running it

```sh
tools/ant_logger.py /dev/cu.usbmodemXXXX              # find whatever is nearest
tools/ant_logger.py /dev/cu.usbmodemXXXX --type 124   # pin to an SDM footpod
```

Writes `logs/antlog-<timestamp>.jsonl` — one record per received ANT message
(raw hex plus a decode), plus the bridge's own console lines and a periodic
`STATUS`, so the treadmill link and the difficulty scale land in the same
timeline. Ctrl-C closes the channel.

With no `--type` it searches with a wildcard id, reports what it locked onto,
and then **re-opens at that profile's message period**. The periods differ —
SDM 8134, HRM 8070, most others 8192 — and a slave whose period disagrees with
the master it found receives the first message and then drops most of the rest,
which looks exactly like a sensor with terrible reception.

The console command underneath is `ANTLOG`:

```
ANTLOG                              status
ANTLOG START [type [dev [period [freq]]]]    0 = wildcard / default
ANTLOG STOP
```

## Two things that are load-bearing

- **The search runs at low priority** (high-priority search timeout 0,
  low-priority timeout 255 = infinite). High-priority search blocks other
  channels' timeslots outright, and the channel it would block is the footpod
  broadcast the watch is recording — the reference side of the comparison. The
  infinite low-priority timeout is what lets you start the logger before
  getting on the treadmill: any finite value closes the channel silently after
  ~25 s of not hearing a sleeping sensor, and "no lines" then reads as a broken
  tool.
- **Closing an ANT channel is asynchronous.** `sd_ant_channel_close()` returns
  immediately; the channel is not down until `EVENT_CHANNEL_CLOSED`. Re-opening
  inside one call therefore fails with `CHANNEL_IN_WRONG_STATE` (0x4015), which
  is what the first version did on every retune — it took the channel down and
  the log went quiet with the error visible only in the command reply.
  `ant_log_start()` now defers the re-open into the close event, and the logger
  re-issues `ANTLOG START` if a periodic status reports the channel not running.

## First capture: a real treadmill SDM sensor, 2026-08-18

`logs/antlog-20260818T175140.jsonl` — 108 consecutive messages, **zero radio
dropouts**, belt at ~10 km/h. Device 19316, type 124, trans type 5, ~4 Hz.

| | that sensor | our broadcast (`ant_sdm.c` / `ant_sdm_encode.c`) |
|---|---|---|
| page mix | 1 and 2 alternating ~1:1 (75/75), 80 and 81 occasionally | 60 page 1 per 64-slot cycle; 2 page 2, one each of 80/81/22 |
| page 1 time field | **always 0** | the footpod clock, integer + 1/256 s fraction |
| page 1 strides | **always 0** | 0 (no stride sensor) |
| page 1 update latency | always 0 | 0 |
| page 2 cadence | **valid, 75.0–76.5 spm** | 0xFF / 0xF (the profile's "invalid") |
| page 2 status byte | 0xA1 — use state active, battery level set | 0x01 — use state active only |
| speed | bytes 4 (low nibble) + 5 | same |

Three things fall out of that, none of them yet acted on:

1. **Our page-1 speed encoding is confirmed against a real sensor.** Nordic's
   SDK marks those two bytes `reserved` in its page-1 struct; this sensor puts
   instantaneous speed exactly where `ant_sdm_encode_page1()` does.
2. **A real sensor broadcasts a flat zero time field and a flat zero stride
   count, and watches record it fine.** `ant_sdm_encode.c`'s fractional-time
   work assumes a receiver differentiates distance against page 1's clock. This
   sensor gives a receiver nothing to differentiate against, so at least some
   receivers cannot be doing that — worth re-reading
   `docs/sdm-recording-analysis.md` §4 and §8 (the lap-distance failure the
   clock fix repaired) against, before any more effort goes into the time
   field. The clock fix demonstrably worked for *this* watch; this says it is
   not a universal requirement of the profile.
3. **It sends a real cadence.** We send the profile's invalid encoding because
   the bridge has no stride sensor and must not invent one
   (`CLAUDE.md`, ANT section) — that stays true. But it means the watch has
   never seen a cadence-bearing SDM from us, and this sensor is the control
   case for the zero-cadence investigation: pair the watch to *it* and the same
   recording pipeline can be scored with a known-good sensor in the loop.

## What was changed as a result (2026-08-18, `0818-1802`)

Two things, both of them cases where our broadcast disagreed with the profile
*and* with a real sensor. Neither touches cadence.

- **`SDM_CHANNEL_PERIOD` 8192 → 8134.** The profile allows only 8134 (4.03 Hz)
  and 16268 (2.01 Hz) — `$SDK_ROOT/components/ant/ant_profiles/ant_sdm/ant_sdm.h`
  — and the captured sensor's measured median inter-message time was 248 ms,
  i.e. 8134. `ant_sdm.c` had 8192 under a comment claiming profile compliance.
  A tracking slave re-syncs to the master it hears, so 8192 worked; this is
  about not sitting 0.7% off a certified profile in a way nothing on the watch
  side would ever report.
- **Page 2 byte 6, `0x00` → `0xFF`.** It is a reserved byte. The SDK's own
  encoder writes `UINT8_MAX` and the captured sensor sent `0xFF` in all 687 of
  its page 2s. This is the same class of mistake as the zero clock and the zero
  cadence before it: a well-formed value where "no value" was meant.

⚠ `make pace-test`'s `SDM_CYCLE` is now **pinned to 16.0** in the Makefile. The
default .FIT was recorded at 8192 (64 × 0.25 s), so reading the period out of
`ant_sdm.c` would model that trace with a period its recording firmware never
used. Drop the pin back to empty once the default trace is re-recorded on 8134
firmware. The gated metrics are unaffected either way (verified both ways); the
pin matters for the hole/phase attribution, which is not gated.

## What was deliberately NOT changed

- **The invalid cadence encoding.** The bridge has no stride sensor and must
  not invent one. That the captured sensor sends a valid cadence does not give
  us one to send.
- **The page mix.** The captured sensor alternates page 1 and page 2 ~1:1; we
  send 60 page 1s per 64-slot cycle. Both pages carry speed, so the mix is not
  obviously wrong, and the current spread is the fix for a measured speed-hole
  regression (`docs/sdm-recording-analysis.md` §10a). No evidence favours
  changing it.
- **The page 1 clock.** The captured sensor sends a flat zero and works; ours
  sends a real clock and that demonstrably repaired lap distance (§8). Sending
  more truthful data than the profile minimum is not a defect.

## The control experiment — run 2026-08-18, and it answered the question

The open bridge issue is cadence: recordings show `avg_running_cadence` 252 with
`max` 0, and three separate attempts (invalid encoding, page 22, page 70
responses) failed to fix it. Two explanations survived — the watch consumes
page 2's cadence field, or it ignores capabilities and derives cadence some
other way — and a .FIT recorded from our own footpod cannot tell them apart.

The control: a fenix 8 recorded the real sensor (device 19316) for the whole
run, while the bridge sniffed the same sensor off the air. Same sensor, same
belt, same hour, one recording made by a watch and one by us.

    tools/compare_sdm_traces.py REF.fit --antlog logs/antlog-*.jsonl

Result (fenix 8 activity 24029215957 vs `antlog-20260818T180410.jsonl`, 2308 s
of overlap):

| | recorded by the watch | broadcast on the air |
|---|---|---|
| cadence, mean | 76.107 spm | 75.757 spm |
| cadence, samples within 0.5 spm | **95.4 %** (1594/1671, at a 1 s offset) | |
| speed, mean | 2.853 m/s | 2.859 m/s |
| speed, RMSE | **0.013 m/s** at the same offset | |

The one-second offset is the watch recording the last page it received, and it
applies to speed and cadence alike. At that offset the recorded cadence *is* the
broadcast cadence, sample for sample.

**So the watch reads page 2's cadence field, and our `0xFF` is what lands in a
recording as the 252 / max-0 garbage.** The sensor's own FIT confirms the
identity end to end: `device_info` names `north_pole_engineering` with a serial
number identical to the one the sniffer decoded out of that sensor's own common
page 81 — the recording and the air trace are the same physical sensor, not two
plausible ones. (The literal serial is deliberately not reproduced here, for the
same reason the .FITs are untracked.)

## Both watches, one belt — 2026-08-18

The other half of the same run: a fenix 6X recorded **our** footpod while the
fenix 8 recorded the Runn. 2032 s of genuine overlap.

    tools/compare_sdm_traces.py REF.fit BRIDGE.fit

| over the common window | fenix 8 / Runn | fenix 6X / bridge |
|---|---|---|
| speed, mean | 2.857 m/s | 2.817 m/s — **−1.40 %** |
| distance, summed | 5181.8 m | 5109.9 m — **−1.39 %** |
| cadence, mean | 76.5 spm | **0.0 spm, in all 260 samples** |
| session cadence | avg 75 / max 83 / 4680 strides | **avg 253 / max 0** / 2361 strides |
| self-consistency | 1.0001 | 1.0020 |

Three things worth keeping:

- **The speed and distance deficits agree to 0.01 %.** That is the signature of
  a scale error, not a lag or a dropout: the belt speed iFit reports — which is
  all our footpod can broadcast — is about 1.4 % below what the Runn measures
  optically. Both recordings are internally consistent (a file's distance
  advances at the rate its own speed claims, to 0.2 %), so neither watch is
  confused; the two sensors simply disagree about the same belt by 1.4 %.
- **Our cadence lands as literally zero in every record**, and as the familiar
  avg 253 / max 0 at session level. Against the control above — where the
  watch's recorded cadence tracked the Runn's broadcast page 2 to 95.4 % — this
  closes the loop: the watch reads page 2's cadence field, so it reads ours,
  and ours says nothing usable.
- One confound, named: the 6X also had the Runn attached as **FE-C**
  (`device_info` device_index 4, `fitness_equipment`) alongside our footpod.
  Its speed is 1.4 % off the Runn's own SDM figure, so it is not coming from
  the Runn on either profile — but a future comparison should pair the
  bridge-side watch to the bridge alone.

⚠ The 6X's battery had gone flat, so it started recording on a reset RTC and
stamped the first five records 2026-04-25 before its clock corrected itself
mid-run (a 115.5-day jump at 22:08:47 UTC). `compare_sdm_traces.py` aligns on
absolute UTC and falls back to start-relative alignment when two recordings do
not overlap at all; here the post-correction 271 records aligned directly. The
pauses are NOT synchronised between the two watches, which is why the distance
comparison sums consecutive common samples instead of differencing endpoints —
done the naive way it reported the bridge running 0.6 % LONG while its speed
read 1 % short.

The same conclusion falls out of the recording history independently:
`docs/sdm-recording-analysis.md` §14 tabulates every .FIT and shows the
signature is present in the first run our footpod ever appeared in — ten days
and four recordings before page 22 — and unmoved by page 22 and page 70 alike.
The only variable that has ever changed it is which sensor the watch is paired
to.

That makes the next change a decision rather than a guess. The bridge still has
no stride sensor and still must not invent a cadence — but it now has a third
option it did not have before: **stop broadcasting page 2 at all**, so the watch
has no SDM cadence field to read and falls back to the wrist cadence that
`docs/sdm-recording-analysis.md` §12.2 shows is correct and available. Page 2's
slots would return to page 1, which carries speed anyway. Not done here: it
costs a full run to verify, and the run is the user's.
