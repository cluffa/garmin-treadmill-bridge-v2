#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["garmin-fit-sdk>=21.141"]
# ///
"""Compare what two watches recorded from two different SDM sensors on one belt.

Built for the control experiment in docs/ant-logging.md: one watch records our
synthesized footpod (the bridge), another records a real ANT+ SDM sensor on the
same treadmill at the same time. Two recordings of one belt, from two sensors —
so every difference between them is a property of the sensors, not of the run.

    tools/compare_sdm_traces.py REF.fit BRIDGE.fit
    tools/compare_sdm_traces.py REF.fit --antlog logs/antlog-*.jsonl

`REF` is the reference recording (the real sensor). The second positional
argument is the bridge-side recording. `--antlog` adds a third trace: the raw
ANT pages the bridge itself sniffed off the reference sensor (firmware/ant_log.c),
which is what lets you tell whether a watch sourced a field FROM the sensor or
computed it itself — a recorded value that tracks the broadcast page came off
the air; one that merely looks plausible did not.

Alignment is by absolute UTC, not by record index: both watches stamp real time,
and a session with auto-pause has gaps that index alignment would smear. The
runs do not have to start together — only overlap. A small cross-correlation
then refines the offset and reports it as a lag, WITH a confidence signal: on a
near-steady trace there is nothing for a correlation to lock onto, and a lag
estimated from a flat line is noise. Read the confidence before the lag.
"""

import argparse
import json
import math
import statistics
import sys
from datetime import datetime, timezone

MAX_SHIFT_S = 30
MAX_SHIFT_START_S = 300


def load_fit(path):
    from garmin_fit_sdk import Decoder, Stream
    msgs, _ = Decoder(Stream.from_file(path)).read()
    speed, cad, dist = {}, {}, {}
    for m in msgs.get("record_mesgs", []):
        ts = m.get("timestamp")
        if ts is None:
            continue
        t = int(ts.replace(tzinfo=timezone.utc).timestamp())
        sp = m.get("enhanced_speed", m.get("speed"))
        if sp is not None:
            speed[t] = float(sp)
        c = m.get("cadence")
        if c is not None:
            # A running watch stores strides/min per foot; fractional_cadence
            # carries the half-step the integer field cannot.
            cad[t] = float(c) + float(m.get("fractional_cadence") or 0.0)
        d = m.get("distance")
        if d is not None:
            dist[t] = float(d)
    sess = (msgs.get("session_mesgs") or [{}])[0]
    devs = []
    for m in msgs.get("device_info_mesgs", []):
        if m.get("source_type") == "antplus":
            key = (m.get("manufacturer"), m.get("serial_number"),
                   m.get("antplus_device_type"))
            if key not in devs:
                devs.append(key)
    return {"path": path, "speed": speed, "cadence": cad, "distance": dist,
            "session": sess, "ant": devs}


def load_antlog(path):
    """Per-second speed and cadence as the bridge decoded them off the air."""
    speed, cad = {}, {}
    for line in open(path):
        r = json.loads(line)
        if r.get("kind") != "msg":
            continue
        t = int(datetime.fromisoformat(r["t"]).timestamp())
        if r.get("page") == 1 and "speed_mps" in r:
            speed.setdefault(t, []).append(r["speed_mps"])
        if r.get("page") == 2:
            if r.get("cadence_spm") is not None:
                cad.setdefault(t, []).append(r["cadence_spm"])
            if "speed_mps" in r:
                speed.setdefault(t, []).append(r["speed_mps"])
    # ~4 messages land in each 1 s bucket; the mean is the like-for-like
    # comparison against a watch that records one sample per second.
    return {"path": path,
            "speed": {t: statistics.fmean(v) for t, v in speed.items()},
            "cadence": {t: statistics.fmean(v) for t, v in cad.items()},
            "distance": {}, "session": {}, "ant": []}


def overlap(a, b):
    return sorted(set(a) & set(b))


def rebase(tr):
    """Re-stamp a trace as seconds from its own first sample.

    Absolute UTC is the right alignment when both watches know the time. A
    watch whose battery went flat does not: it comes back with whatever its RTC
    reset to, and stamps a recording months away from when it happened. Nothing
    inside the file says so -- the timestamps are perfectly self-consistent,
    just wrong -- so alignment has to fall back to "seconds since this
    recording started", and the offset between the two starts becomes something
    to search for rather than something to read off.
    """
    out = dict(tr)
    t0 = min([min(tr[f]) for f in ("speed", "cadence", "distance") if tr[f]],
             default=None)
    if t0 is None:
        return out
    for f in ("speed", "cadence", "distance"):
        out[f] = {t - t0: v for t, v in tr[f].items()}
    out["t0"] = t0
    return out


def stats(a, b, keys):
    d = [a[t] - b[t] for t in keys]
    return {
        "n": len(keys),
        "mean_diff": statistics.fmean(d),
        "rmse": math.sqrt(statistics.fmean(x * x for x in d)),
        "p95_abs": sorted(abs(x) for x in d)[int(0.95 * (len(d) - 1))],
        "ref_mean": statistics.fmean(a[t] for t in keys),
        "cmp_mean": statistics.fmean(b[t] for t in keys),
    }


def best_shift(ref, cmp_, max_shift=MAX_SHIFT_S):
    """Shift cmp_ in whole seconds to minimise RMSE against ref.

    Returns (shift, rmse_at_shift, rmse_at_zero, spread). `spread` is the
    standard deviation of the reference over the overlap: it is the signal the
    shift is estimated FROM. Near zero, any shift fits as well as any other and
    the number means nothing.
    """
    base = overlap(ref, cmp_)
    if len(base) < 10:
        return None
    spread = statistics.pstdev([ref[t] for t in base])
    best = None
    for s in range(-max_shift, max_shift + 1):
        shifted = {t - s: v for t, v in cmp_.items()}
        keys = overlap(ref, shifted)
        if len(keys) < len(base) // 2:
            continue
        r = math.sqrt(statistics.fmean((ref[t] - shifted[t]) ** 2 for t in keys))
        if best is None or r < best[1]:
            best = (s, r)
    zero = math.sqrt(statistics.fmean((ref[t] - cmp_[t]) ** 2 for t in base))
    return {"shift_s": best[0], "rmse": best[1], "rmse_at_zero": zero,
            "ref_spread": spread}


def fmt_window(keys, align="utc"):
    span = "(%d s of overlap, %d samples)" % (keys[-1] - keys[0] + 1, len(keys))
    if align != "utc":
        # Relative mode: these are seconds into the reference recording, and
        # rendering them as a clock would invent a time neither file claims.
        return "t+%d s -> t+%d s  %s" % (keys[0], keys[-1], span)
    lo = datetime.fromtimestamp(keys[0], timezone.utc).astimezone()
    hi = datetime.fromtimestamp(keys[-1], timezone.utc).astimezone()
    return "%s -> %s  %s" % (lo.strftime("%H:%M:%S"), hi.strftime("%H:%M:%S"), span)


def self_consistency(tr):
    """Ratio of a trace's own distance advance to the integral of its own speed.

    Contiguous sample pairs only (<=15 s apart, both moving), so a timer pause
    or a clock jump cannot contaminate it.
    """
    ts = sorted(set(tr["speed"]) & set(tr["distance"]))
    num = den = 0.0
    n = 0
    for t0, t1 in zip(ts, ts[1:]):
        dt = t1 - t0
        if not 0 < dt <= 15:
            continue
        sp = (tr["speed"][t0] + tr["speed"][t1]) / 2
        if sp < 0.5:
            continue
        num += tr["distance"][t1] - tr["distance"][t0]
        den += sp * dt
        n += 1
    return (num / den, n) if den else None


def describe(tr):
    s = tr["session"]
    if not s:
        return
    print("  %s" % tr["path"])
    if tr["ant"]:
        for mfg, ser, typ in tr["ant"]:
            print("    ANT+ sensor: %s serial=%s type=%s" % (mfg, ser, typ))
    else:
        print("    ANT+ sensor: none recorded")
    st = s.get("start_time")
    if st:
        print("    claims to have started: %s" % st.astimezone().strftime(
            "%Y-%m-%d %H:%M:%S %Z"))
    print("    session: dist=%.0f m  elapsed=%.0f s  cadence avg=%s max=%s "
          "strides=%s" % (s.get("total_distance") or 0,
                          s.get("total_elapsed_time") or 0,
                          s.get("avg_running_cadence"),
                          s.get("max_running_cadence"),
                          s.get("total_strides")))
    ic = self_consistency(tr)
    if ic:
        # Does the file's distance advance at the rate its own speed claims?
        # Asked before comparing anything across files, because a sensor whose
        # two fields disagree with EACH OTHER is a different problem from two
        # sensors disagreeing, and the second question is unanswerable until
        # the first is settled.
        print("    self-consistency: distance/(own speed x dt) = %.4f over %d "
              "pairs (%+.2f%%)" % (ic[0], ic[1], 100 * (ic[0] - 1)))


def apply_shift(tr, shift):
    if not shift:
        return tr
    out = dict(tr)
    for f in ("speed", "cadence", "distance"):
        out[f] = {t - shift: v for t, v in tr[f].items()}
    return out


def compare(ref, cmp_, label, max_shift=MAX_SHIFT_S, align="utc"):
    print("\n" + "=" * 72)
    print("%s   (reference: %s)" % (label, ref["path"].split("/")[-1]))
    print("=" * 72)
    print("alignment: %s" % align)

    for field in ("speed", "cadence"):
        a, b = ref[field], cmp_[field]
        keys = overlap(a, b)
        print("\n-- %s" % field)
        if len(keys) < 10:
            print("   no usable overlap (%d common seconds)" % len(keys))
            continue
        print("   window: %s" % fmt_window(keys, align))
        st = stats(a, b, keys)
        unit = "m/s" if field == "speed" else "spm"
        print("   reference mean %.3f %s   compared mean %.3f %s"
              % (st["ref_mean"], unit, st["cmp_mean"], unit))
        print("   mean difference %+.3f %s   RMSE %.3f   p95|diff| %.3f"
              % (st["mean_diff"], unit, st["rmse"], st["p95_abs"]))
        if field == "speed" and st["ref_mean"]:
            print("   scale factor  compared/reference = %.4f  (%+.2f%%)"
                  % (st["cmp_mean"] / st["ref_mean"],
                     100 * (st["cmp_mean"] / st["ref_mean"] - 1)))

        bs = best_shift(a, b, max_shift)
        if bs:
            # A lag is only meaningful if the reference actually moves. On a
            # steady belt the correlation surface is flat and the argmin is
            # noise, so say which case this is instead of printing a number
            # that looks like a measurement.
            conf = "usable" if bs["ref_spread"] > 0.15 else "LOW — trace is near-steady"
            print("   best shift %+d s (RMSE %.3f vs %.3f unshifted); "
                  "reference spread %.3f -> confidence: %s"
                  % (bs["shift_s"], bs["rmse"], bs["rmse_at_zero"],
                     bs["ref_spread"], conf))

        if field == "cadence":
            # Did the watch take this off the air, or make it up? Compare the
            # recorded value to the broadcast one sample by sample; a sourced
            # field matches to within rounding, including through the zeros.
            #
            # Report the match rate at the best shift as well as at zero. A
            # watch records the last page it received, so a one-second offset
            # between the air and the record is expected and is not evidence
            # against sourcing -- scoring only at zero shift would understate a
            # perfect match as a mediocre one.
            def match_rate(off):
                sh = {t - off: v for t, v in b.items()}
                ks = overlap(a, sh)
                n = sum(1 for t in ks if abs(a[t] - sh[t]) < 0.51)
                return n, len(ks)
            n0, d0 = match_rate(0)
            print("   within 0.5 spm, unshifted: %d/%d (%.1f%%)"
                  % (n0, d0, 100 * n0 / d0))
            if bs and bs["shift_s"]:
                ns, ds = match_rate(bs["shift_s"])
                print("   within 0.5 spm, at %+d s:  %d/%d (%.1f%%)"
                      % (bs["shift_s"], ns, ds, 100 * ns / ds))
            zeros = [t for t in keys if a[t] == 0 or b[t] == 0]
            zmatch = sum(1 for t in zeros if abs(a[t] - b[t]) < 0.51)
            if zeros:
                print("   zero-cadence samples: %d, of which both agree: %d"
                      % (len(zeros), zmatch))

    ka, kb = ref["distance"], cmp_["distance"]
    keys = overlap(ka, kb)
    if len(keys) > 60:
        # Summed over CONSECUTIVE common samples, not endpoint-to-endpoint.
        # Two watches auto-pause at different moments: whichever one is still
        # recording accumulates distance the other never sees, and an endpoint
        # difference silently attributes that to the sensor. This trace had
        # exactly that — a 151 s stop on one watch and ~738 s of timer stops on
        # the other, at different times — and endpoint distance said the bridge
        # ran 0.6% LONG while its speed read 1% SHORT, which cannot both be a
        # property of the sensor.
        da = db = 0.0
        span = 0
        for t0, t1 in zip(keys, keys[1:]):
            dt = t1 - t0
            if dt > 15:
                continue
            da += ka[t1] - ka[t0]
            db += kb[t1] - kb[t0]
            span += dt
        print("\n-- distance, summed over %d s of common recording" % span)
        print("   reference %.1f m   compared %.1f m   ratio %.4f  (%+.2f%%)"
              % (da, db, db / da if da else float("nan"),
                 100 * (db / da - 1) if da else float("nan")))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ref_fit")
    ap.add_argument("cmp_fit", nargs="?")
    ap.add_argument("--antlog")
    ap.add_argument("--align", choices=("utc", "start", "auto"), default="auto",
                    help="utc: absolute timestamps (correct when both clocks "
                         "are right). start: seconds from each recording's own "
                         "first sample, for a watch whose battery died and came "
                         "back with a wrong RTC. auto (default): utc if the two "
                         "recordings overlap in absolute time, start if they "
                         "do not.")
    ap.add_argument("--shift-s", type=int, default=None,
                    help="force this offset (seconds to subtract from the "
                         "compared trace) instead of searching for one")
    args = ap.parse_args()

    if not args.cmp_fit and not args.antlog:
        ap.error("give a second .fit to compare, or --antlog, or both")

    ref = load_fit(args.ref_fit)
    print("traces")
    describe(ref)

    if args.cmp_fit:
        cmp_ = load_fit(args.cmp_fit)
        describe(cmp_)

        align = args.align
        if align == "auto":
            common = overlap(ref["speed"], cmp_["speed"])
            align = "utc" if len(common) >= 60 else "start"
            if align == "start":
                print("\n  ! the two recordings do not overlap in absolute "
                      "time (%d common seconds)." % len(common))
                print("    Falling back to start-relative alignment — one of "
                      "these clocks is wrong.")
                print("    Their stamped starts differ by %.1f days."
                      % (abs(min(ref["speed"], default=0)
                             - min(cmp_["speed"], default=0)) / 86400.0))

        a, b = (ref, cmp_) if align == "utc" else (rebase(ref), rebase(cmp_))
        window = MAX_SHIFT_S if align == "utc" else MAX_SHIFT_START_S
        if args.shift_s is not None:
            b = apply_shift(b, args.shift_s)
            print("\n  forced shift: %+d s" % args.shift_s)
            window = MAX_SHIFT_S
        compare(a, b, "RECORDED vs RECORDED — two watches, two sensors, one belt",
                max_shift=window, align=align)

    if args.antlog:
        al = load_antlog(args.antlog)
        print("  %s" % al["path"])
        print("    %d speed seconds, %d cadence seconds decoded off the air"
              % (len(al["speed"]), len(al["cadence"])))
        compare(ref, al,
                "RECORDED vs BROADCAST — did the watch take these off the air?",
                align="utc")

    return 0


if __name__ == "__main__":
    sys.exit(main())
