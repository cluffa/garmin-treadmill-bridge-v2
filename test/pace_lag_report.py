#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["garmin-fit-sdk>=21.141"]
# ///
"""
pace_lag_report.py — score how fast, and how faithfully, the belt follows the
watch's workout targets.

Run a structured interval workout with the bridge in SDM:TGT mode (the footpod
broadcasts the bridge's *resolved target*, so the recorded speed trace is
exactly what the bridge commanded, with the whole watch->BLE->bridge->ANT->watch
pipeline delay baked in), download the .FIT, and point this at it:

    ./test/pace_lag_report.py test/23806153959_ACTIVITY.fit

It reconstructs what the bridge *should* have commanded at every instant --
straight from the workout definition and the per-step laps in the same file,
running the same decode policy as core/workout_ctrl.c -- and scores the
recorded trace against it.

--- what it measures ------------------------------------------------------

Reference command c(t) and recorded trace a(t) are both piecewise-constant, so
every integral below is exact, not a quadrature approximation.

  IAE   = integral |a(t) - c(t)| dt          [m/s * s = metres]
  ISE   = integral (a(t) - c(t))^2 dt
  bias  = integral (a(t) - c(t)) dt          [signed metres of over/under-run]

The headline number is derived from IAE. For a pure transport delay tau on a
step of size |dc|, the area between the curves is exactly |dc| * tau. So

  effective_lag_s = IAE / sum(|dc_i|)

is a lag in seconds that also absorbs dropouts and level errors -- one number,
in seconds, that goes down when and only when the belt tracks better. IAE is
decomposed into transient / dropout / steady parts so a regression can be
attributed rather than just observed.

Alongside it, two independent lag estimates that should agree with it:
  * per-transition edge lag  -- when the trace crosses the midpoint of the step
  * best global shift tau*   -- argmin over tau of IAE(c shifted by tau),
                                found on a fine grid; sub-second, unlike the
                                1 Hz edge measurement

--- reproducibility -------------------------------------------------------

    ./test/pace_lag_report.py FILE.fit --json out.json
    ./test/pace_lag_report.py FILE.fit --write-baseline test/baselines/x.json
    ./test/pace_lag_report.py FILE.fit --baseline test/baselines/x.json

The last form is the regression gate: it compares against the stored numbers
and exits non-zero if any metric got worse by more than its tolerance.

Note both baseline and candidate must come from the *same workout*; this
compares firmware revisions, not workouts.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from bisect import bisect_right
from dataclasses import dataclass, field
from pathlib import Path

try:
    from garmin_fit_sdk import Decoder, Stream
except ImportError:  # pragma: no cover - uv installs this
    sys.exit("garmin-fit-sdk missing; run this file directly so uv installs it")

REPO = Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Bridge policy mirror. Kept in lockstep with core/workout_ctrl.c decode_action.
# ---------------------------------------------------------------------------

DEFAULT_REST_KMH = 4.0
#: two speeds within this are "the same target" (SPEED_EPS_KMH in workout_ctrl.c)
SPEED_EPS_KMH = 0.05


def firmware_rest_kmh() -> float:
    """Read REST_SPEED_KMH out of core/workout_ctrl.c so the model cannot
    silently drift from the firmware it is scoring."""
    src = REPO / "core" / "workout_ctrl.c"
    try:
        m = re.search(r"#define\s+REST_SPEED_KMH\s+([0-9.]+)f?", src.read_text())
        if m:
            return float(m.group(1))
    except OSError:
        pass
    return DEFAULT_REST_KMH


def firmware_sdm_cycle_s() -> float | None:
    """Length of the ANT SDM page-rotation cycle, from firmware/ant_sdm.c.

    Speed dropouts that all land at the same phase of this cycle are the
    background pages (80/81/page 2) crowding page 1 out of a whole one-second
    window -- which is what the watch samples. Parsed rather than hardcoded so
    the diagnostic follows the firmware; pass --sdm-cycle-s to score a trace
    recorded by a *different* firmware revision.
    """
    src = REPO / "firmware" / "ant_sdm.c"
    try:
        text = src.read_text()
        n = re.search(r"#define\s+CYCLE_LEN\s+(\d+)", text)
        p = re.search(r"#define\s+SDM_CHANNEL_PERIOD\s+(\d+)", text)
        if n and p:
            return int(n.group(1)) * int(p.group(1)) / 32768.0
    except OSError:
        pass
    return None


def f32(x: float) -> float:
    """Round to float32, matching the firmware's float arithmetic."""
    import struct

    return struct.unpack("f", struct.pack("f", x))[0]


def decode_action(step: dict, rest_kmh: float | None) -> tuple[str, float]:
    """core/workout_ctrl.c decode_action(), in Python.

    Returns ("speed", mps) | ("none", 0.0). "none" means *hold the last
    command* -- it is not a stop. Timer state is handled by the caller.

    rest_kmh=None models the pre-4932511 firmware, where a rest step with no
    speed target of its own fell through to ACT_NONE and the belt held the
    work-interval pace.
    """
    tt = step.get("target_type")
    if tt == "speed":
        # The watch packs mm/s with a truncating toNumber(); prefer the raw
        # integer FIT field, which is already mm/s.
        low = step.get("custom_target_value_low")
        high = step.get("custom_target_value_high")
        if low is None and step.get("custom_target_speed_low") is not None:
            low = int(step["custom_target_speed_low"] * 1000.0)
        if high is None and step.get("custom_target_speed_high") is not None:
            high = int(step["custom_target_speed_high"] * 1000.0)
        low, high = int(low or 0), int(high or 0)
        mmps = (low + high) // 2 if (low and high) else (low or high)
        if mmps:
            return "speed", f32(f32(mmps * 0.0036) / 3.6)  # mm/s -> km/h -> m/s
    if step.get("intensity") == "rest" and rest_kmh is not None:
        return "speed", f32(rest_kmh / 3.6)
    return "none", 0.0


def sdm_quantize(mps: float) -> float:
    """What core/ant_sdm_encode.c actually puts on the wire: integer m/s plus a
    1/256 m/s fraction. Explains a few mm/s of steady-state offset that is
    encoder resolution, not tracking error."""
    if mps <= 0:
        return 0.0
    i = math.floor(mps)
    frac = min(255, round((mps - i) * 256.0))
    return i + frac / 256.0


# ---------------------------------------------------------------------------
# Piecewise-constant signal: exact integrals, no resampling.
# ---------------------------------------------------------------------------


@dataclass
class Signal:
    """A right-continuous step function. ts[i] is where value vs[i] starts;
    it holds until ts[i+1]. Defined on [ts[0], t_end]."""

    ts: list[float]
    vs: list[float | None]
    t_end: float

    def at(self, t: float) -> float | None:
        if t < self.ts[0] or t > self.t_end:
            return None
        return self.vs[bisect_right(self.ts, t) - 1]

    def breakpoints(self) -> list[float]:
        return self.ts + [self.t_end]

    def shifted(self, tau: float) -> "Signal":
        return Signal([t + tau for t in self.ts], list(self.vs), self.t_end + tau)


def integrate(a: Signal, c: Signal, t0: float, t1: float, power: int = 1,
              signed: bool = False, mask=None) -> float:
    """Integral over [t0, t1] of |a - c|^power (or the signed difference).

    Both operands are piecewise constant, so the integral is an exact finite
    sum over the merged breakpoints. Intervals where either signal is
    undefined (a dropped sample, or outside the common support) contribute
    nothing; `mask(t_mid)` can veto an interval so the total can be split into
    transient / dropout / steady parts that sum back to the whole.
    """
    cuts = sorted({t for t in a.breakpoints() + c.breakpoints() if t0 <= t <= t1}
                  | {t0, t1})
    total = 0.0
    for lo, hi in zip(cuts, cuts[1:]):
        if hi <= lo:
            continue
        mid = 0.5 * (lo + hi)
        if mask is not None and not mask(mid):
            continue
        av, cv = a.at(mid), c.at(mid)
        if av is None or cv is None:
            continue
        d = av - cv
        total += (d if signed else abs(d) ** power) * (hi - lo)
    return total


# ---------------------------------------------------------------------------
# FIT ingest
# ---------------------------------------------------------------------------


@dataclass
class Run:
    name: str
    t0: object
    rec_t: list[float]
    rec_v: list[float | None]
    rec_d: list[float | None]
    steps: dict[int, dict]
    laps: list[dict]  # {t_start, t_end, step_index, intensity}
    timer_on: float
    timer_off: float


def load(path: Path) -> Run:
    msgs, errors = Decoder(Stream.from_file(str(path))).read()
    if errors:
        print(f"warning: {len(errors)} FIT decode errors", file=sys.stderr)

    recs = msgs.get("record_mesgs") or []
    if not recs:
        sys.exit(f"{path}: no record messages")
    t0 = recs[0]["timestamp"]
    secs = lambda ts: (ts - t0).total_seconds()

    rec_t, rec_v, rec_d = [], [], []
    for r in recs:
        rec_t.append(secs(r["timestamp"]))
        v = r.get("enhanced_speed")
        if v is None:
            v = r.get("speed")
        rec_v.append(None if v is None else float(v))
        d = r.get("distance")
        rec_d.append(None if d is None else float(d))

    steps = {int(s["message_index"]): s for s in msgs.get("workout_step_mesgs") or []}

    laps = []
    for lap in msgs.get("lap_mesgs") or []:
        idx = lap.get("wkt_step_index")
        if idx is None or lap.get("start_time") is None:
            continue
        st = secs(lap["start_time"])
        dur = lap.get("total_elapsed_time") or lap.get("total_timer_time") or 0.0
        laps.append({"t_start": st, "t_end": st + float(dur),
                     "step_index": int(idx), "intensity": lap.get("intensity")})
    laps.sort(key=lambda l: l["t_start"])
    if not laps:
        sys.exit(f"{path}: no laps carry wkt_step_index -- not a structured workout")

    timer_on, timer_off = rec_t[0], rec_t[-1]
    for e in msgs.get("event_mesgs") or []:
        if e.get("event") != "timer":
            continue
        if e.get("event_type") == "start":
            timer_on = min(timer_on, secs(e["timestamp"]))
        elif e.get("event_type") in ("stop", "stop_all"):
            timer_off = secs(e["timestamp"])

    name = (msgs.get("workout_mesgs") or [{}])[0].get("wkt_name") or path.stem
    return Run(name, t0, rec_t, rec_v, rec_d, steps, laps, timer_on, timer_off)


def command_signal(run: Run, rest_kmh: float | None) -> tuple[Signal, list[dict]]:
    """Reconstruct what the bridge was commanding, second by second.

    ACT_NONE latches: the belt holds the previous command, exactly as
    workout_ctrl_on_frame() does by returning early.
    """
    ts, vs, held = [], [], 0.0
    for lap in run.laps:
        step = run.steps.get(lap["step_index"], {})
        # The lap's own intensity is authoritative for the portion that ran
        # (an interval step resolves to work or rest per repetition).
        merged = dict(step)
        if lap.get("intensity") is not None:
            merged["intensity"] = lap["intensity"]
        kind, mps = decode_action(merged, rest_kmh)
        if kind == "speed":
            held = mps
        ts.append(lap["t_start"])
        vs.append(held)
    t_end = min(run.laps[-1]["t_end"], run.timer_off)
    return Signal(ts, vs, t_end), run.laps


def actual_signal(run: Run) -> Signal:
    """The recorded trace as a step function. A record at t is the value the
    watch had at t and holds until the next record."""
    return Signal(list(run.rec_t), list(run.rec_v), run.rec_t[-1] + 1.0)


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------

MAX_LAG_SEARCH = 12.0     # s -- beyond this a transition counts as missed
DROP_FRACTION = 0.55      # a<55% of commanded, off-transient, is a dropout


@dataclass
class Transition:
    t: float
    from_v: float
    to_v: float
    lag: float | None = None      # edge lag, s (None = never arrived)
    iae: float = 0.0              # area attributable to this transition
    settled: float | None = None  # when |a-c| first stays inside the deadband


@dataclass
class Report:
    file: str
    workout: str
    rest_policy: str
    duration_s: float = 0.0
    n_transitions: int = 0
    transitions: list[Transition] = field(default_factory=list)
    metrics: dict = field(default_factory=dict)
    dropouts: list[dict] = field(default_factory=list)
    holes: list[dict] = field(default_factory=list)
    phase: dict | None = None


def find_transitions(cmd: Signal, eps: float) -> list[Transition]:
    out = []
    for i in range(1, len(cmd.ts)):
        a, b = cmd.vs[i - 1], cmd.vs[i]
        if abs(b - a) > eps:
            out.append(Transition(cmd.ts[i], a, b))
    return out


def measure_edge_lags(tr: list[Transition], act: Signal, run: Run) -> None:
    """Lag = first record at which the trace has crossed the midpoint between
    the old and the new commanded level, in the direction of the change.

    The midpoint is robust to the two known steady-state offsets (SDM 1/256
    quantization and the watch's footpod calibration factor), which shift the
    plateaus by a few mm/s but nowhere near half a step.
    """
    for t in tr:
        mid = 0.5 * (t.from_v + t.to_v)
        up = t.to_v > t.from_v
        t.lag = None
        for ts, v in zip(run.rec_t, run.rec_v):
            if ts < t.t:
                continue
            if ts > t.t + MAX_LAG_SEARCH:
                break
            if v is None:
                continue
            if (v >= mid) if up else (v <= mid):
                t.lag = ts - t.t
                break


def find_dropouts(cmd: Signal, run: Run, tr: list[Transition]) -> list[dict]:
    """A dropout is a sample where the trace collapses (missing, or well below
    the commanded speed) while nowhere near a commanded change.

    Distance is checked across the hole: the SDM distance field is integrated
    from the same target the speed field carries, so if distance keeps
    advancing at the commanded rate the belt never slowed -- the speed sample
    itself was lost or invalidated. That distinguishes an ANT/telemetry glitch
    from the bridge actually dropping the target.
    """
    transient = []
    for t in tr:
        transient.append((t.t, t.t + (t.lag if t.lag is not None else MAX_LAG_SEARCH) + 1.0))

    def in_transient(x: float) -> bool:
        return any(lo <= x <= hi for lo, hi in transient)

    out, cur = [], None
    for i, (ts, v) in enumerate(zip(run.rec_t, run.rec_v)):
        c = cmd.at(ts)
        if c is None or c <= 0.1 or in_transient(ts):
            bad = False
        else:
            bad = v is None or v < DROP_FRACTION * c
        if bad:
            if cur is None:
                cur = {"t_start": ts, "t_end": ts, "commanded": c,
                       "samples": [], "i0": i, "i1": i}
            cur["t_end"] = ts
            cur["i1"] = i
            cur["samples"].append(v)
        elif cur is not None:
            out.append(cur)
            cur = None
    if cur is not None:
        out.append(cur)

    for d in out:
        i0, i1 = d.pop("i0"), d.pop("i1")
        d["duration_s"] = d["t_end"] - d["t_start"] + 1.0
        d["min_speed"] = min((s for s in d["samples"] if s is not None), default=None)
        d["missing_samples"] = sum(1 for s in d["samples"] if s is None)
        # Distance continuity: metres actually accumulated across the hole vs
        # metres the commanded speed implies.
        lo, hi = max(0, i0 - 1), min(len(run.rec_t) - 1, i1 + 1)
        d0, d1 = run.rec_d[lo], run.rec_d[hi]
        if d0 is not None and d1 is not None:
            span = run.rec_t[hi] - run.rec_t[lo]
            got = d1 - d0
            want = d["commanded"] * span
            d["distance_kept_pct"] = 100.0 * got / want if want > 0 else None
        else:
            d["distance_kept_pct"] = None
        d["verdict"] = ("telemetry glitch (distance kept advancing)"
                        if d["distance_kept_pct"] is not None
                        and d["distance_kept_pct"] > 90.0
                        else "belt genuinely slowed")
        d["samples"] = [None if s is None else round(s, 3) for s in d["samples"]]
    return out


def find_speed_holes(cmd: Signal, run: Run) -> list[dict]:
    """Every sample where the watch had no usable speed while the bridge was
    commanding one -- a missing field or a hard 0.

    Unlike find_dropouts() this does *not* exclude the windows just after a
    commanded change, because a hole is a hole whatever else is going on, and
    excluding them would hide exactly the ones that inflate a lag measurement.
    """
    holes = []
    for ts, v in zip(run.rec_t, run.rec_v):
        c = cmd.at(ts)
        if c is None or c <= 0.1:
            continue
        if v is None:
            holes.append({"t": ts, "kind": "missing"})
        elif v <= 0.01:
            holes.append({"t": ts, "kind": "zero"})
    return holes


def phase_analysis(holes: list[dict], cycle_s: float | None,
                   source: str = "unknown") -> dict | None:
    """Do the holes cluster at one phase of the ANT page cycle?

    Circular mean and spread of (t mod cycle). A tight cluster is the signature
    of a deterministic page-schedule artefact rather than random RF loss: the
    same slots come round every cycle, so the same one-second window keeps
    losing page 1.
    """
    if not cycle_s or len(holes) < 2:
        return None
    phases = [h["t"] % cycle_s for h in holes]
    ang = [2 * math.pi * p / cycle_s for p in phases]
    cx = sum(math.cos(a) for a in ang) / len(ang)
    cy = sum(math.sin(a) for a in ang) / len(ang)
    r = math.hypot(cx, cy)                       # 1 = perfectly aligned
    mean_phase = (math.atan2(cy, cx) % (2 * math.pi)) * cycle_s / (2 * math.pi)
    # circular sd, converted from radians back to seconds
    sd = (math.sqrt(-2 * math.log(r)) if 0 < r < 1 else 0.0) * cycle_s / (2 * math.pi)
    return {"cycle_s": cycle_s, "cycle_source": source,
            "phases_s": [round(p, 2) for p in phases],
            "mean_phase_s": mean_phase, "concentration": r, "spread_s": sd,
            "clustered": r > 0.9}


def best_shift(act: Signal, cmd: Signal, t0: float, t1: float,
               lo: float = -1.0, hi: float = 8.0, step: float = 0.02) -> tuple[float, float]:
    """argmin over tau of IAE(a, c shifted forward by tau).

    IAE(tau) is piecewise linear in tau for step functions, so a fine grid
    finds the true minimum to within `step`. Returns (tau*, IAE(tau*)).
    """
    best_t, best_v = 0.0, float("inf")
    n = int(round((hi - lo) / step))
    for k in range(n + 1):
        tau = lo + k * step
        v = integrate(act, cmd.shifted(tau), t0, t1)
        if v < best_v:
            best_t, best_v = tau, v
    return best_t, best_v


def analyse(run: Run, rest_kmh: float | None, policy_name: str, path: Path,
            sdm_cycle_s: float | None = None,
            cycle_source: str = "unknown") -> Report:
    cmd, _ = command_signal(run, rest_kmh)
    act = actual_signal(run)

    t0 = max(cmd.ts[0], run.rec_t[0], run.timer_on)
    t1 = min(cmd.t_end, act.t_end, run.timer_off)
    eps = SPEED_EPS_KMH / 3.6

    tr = find_transitions(cmd, eps)
    measure_edge_lags(tr, act, run)
    drops = find_dropouts(cmd, run, tr)

    # --- area decomposition -------------------------------------------------
    # Every second of the run lands in exactly one bucket, so the three areas
    # sum to IAE_total. transient = a window after each commanded change long
    # enough to contain the response; dropout = the flagged holes; steady =
    # everything else, i.e. pure level error.
    windows = [(t.t, t.t + (t.lag if t.lag is not None else MAX_LAG_SEARCH) + 1.0)
               for t in tr]
    drop_spans = [(d["t_start"], d["t_end"] + 1.0) for d in drops]

    in_win = lambda x: any(lo <= x < hi for lo, hi in windows)
    in_drop = lambda x: any(lo <= x < hi for lo, hi in drop_spans)

    iae_total = integrate(act, cmd, t0, t1)
    iae_tr = integrate(act, cmd, t0, t1, mask=in_win)
    iae_drop = integrate(act, cmd, t0, t1, mask=lambda x: in_drop(x) and not in_win(x))
    iae_steady = integrate(act, cmd, t0, t1,
                           mask=lambda x: not in_win(x) and not in_drop(x))
    ise = integrate(act, cmd, t0, t1, power=2)
    signed = integrate(act, cmd, t0, t1, signed=True)

    for t in tr:
        lo = t.t
        hi = t.t + (t.lag if t.lag is not None else MAX_LAG_SEARCH) + 1.0
        t.iae = integrate(act, cmd, lo, min(hi, t1))

    step_sum = sum(abs(t.to_v - t.from_v) for t in tr) or float("nan")
    cmd_dist = integrate(cmd, Signal([t0], [0.0], t1), t0, t1)  # metres commanded

    tau, iae_at_tau = best_shift(act, cmd, t0, t1)

    lags = [t.lag for t in tr if t.lag is not None]
    lags_sorted = sorted(lags)
    pct = lambda p: (lags_sorted[min(len(lags_sorted) - 1,
                                     int(round(p * (len(lags_sorted) - 1))))]
                     if lags_sorted else None)

    # steady-state gain: least-squares a = g*c over settled samples
    num = den = 0.0
    for ts, v in zip(run.rec_t, run.rec_v):
        if v is None or not (t0 <= ts <= t1) or in_win(ts) or in_drop(ts):
            continue
        c = cmd.at(ts)
        if c and c > 0.1:
            num += v * c
            den += c * c
    gain = num / den if den else float("nan")

    holes = [h for h in find_speed_holes(cmd, run) if t0 <= h["t"] <= t1]

    rep = Report(str(path), run.name, policy_name)
    rep.duration_s = t1 - t0
    rep.n_transitions = len(tr)
    rep.transitions = tr
    rep.dropouts = drops
    rep.holes = holes
    rep.phase = phase_analysis(holes, sdm_cycle_s, cycle_source)
    rep.metrics = {
        # headline -- seconds; every error contributes, lower is better
        "effective_lag_s": iae_total / step_sum,
        "transient_lag_s": iae_tr / step_sum,
        "best_shift_s": tau,
        "residual_lag_after_shift_s": iae_at_tau / step_sum,
        "lag_explained_fraction": (1.0 - iae_at_tau / iae_total) if iae_total else 0.0,
        # edge-measured
        "edge_lag_mean_s": (sum(lags) / len(lags)) if lags else None,
        "edge_lag_median_s": pct(0.5),
        "edge_lag_p90_s": pct(0.9),
        "edge_lag_max_s": max(lags) if lags else None,
        "edge_lag_min_s": min(lags) if lags else None,
        "transitions_missed": sum(1 for t in tr if t.lag is None),
        # areas -- metres
        "iae_m": iae_total,
        "iae_transient_m": iae_tr,
        "iae_dropout_m": iae_drop,
        "iae_steady_m": iae_steady,
        "ise_m2_s": ise,
        "signed_error_m": signed,
        "commanded_distance_m": cmd_dist,
        "iae_pct_of_distance": 100.0 * iae_total / cmd_dist if cmd_dist else None,
        # levels
        "steady_gain": gain,
        "steady_gain_error_pct": (gain - 1.0) * 100.0 if den else None,
        # dropouts
        "dropout_count": len(drops),
        "dropout_seconds": sum(d["duration_s"] for d in drops),
        "dropout_worst_depth_pct": max(
            (100.0 * (1.0 - (d["min_speed"] or 0.0) / d["commanded"]) for d in drops),
            default=0.0),
        "speed_holes": len(holes),
    }
    return rep


# ---------------------------------------------------------------------------
# Presentation
# ---------------------------------------------------------------------------


def ascii_plot(run: Run, cmd: Signal, t0: float, t1: float, width: int = 96,
               height: int = 14) -> str:
    vals = [v for v in run.rec_v if v is not None] + [v for v in cmd.vs]
    vmin, vmax = min(vals), max(vals)
    if vmax - vmin < 1e-6:
        vmax = vmin + 1.0
    pad = 0.12 * (vmax - vmin)
    vmin, vmax = max(0.0, vmin - pad), vmax + pad
    grid = [[" "] * width for _ in range(height)]

    def row(v: float) -> int:
        return max(0, min(height - 1,
                          int(round((vmax - v) / (vmax - vmin) * (height - 1)))))

    def col(t: float) -> int:
        return max(0, min(width - 1, int((t - t0) / (t1 - t0) * (width - 1))))

    for x in range(width):
        t = t0 + (t1 - t0) * x / (width - 1)
        c = cmd.at(t)
        if c is not None:
            grid[row(c)][x] = "-"
    for ts, v in zip(run.rec_t, run.rec_v):
        if not (t0 <= ts <= t1):
            continue
        if v is None:
            for y in range(height):
                if grid[y][col(ts)] == " ":
                    grid[y][col(ts)] = ":"
            continue
        grid[row(v)][col(ts)] = "#" if grid[row(v)][col(ts)] == "-" else "*"

    lines = []
    for y, r in enumerate(grid):
        v = vmax - (vmax - vmin) * y / (height - 1)
        lines.append(f"  {v * 3.6:5.2f} |" + "".join(r))
    lines.append("        +" + "-" * width)
    left, right = f"{t0:.0f}s", f"{t1:.0f}s"
    lines.append("         " + left + " " * max(1, width - len(left) - len(right)) + right)
    lines.append("        legend: '-' commanded  '*' recorded  '#' both  ':' sample lost")
    return "\n".join(lines)


def fmt(v, spec="{:.3f}", none="--"):
    return none if v is None else spec.format(v)


def print_report(rep: Report, run: Run, cmd: Signal, verbose: bool) -> None:
    m = rep.metrics
    t0 = max(cmd.ts[0], run.rec_t[0], run.timer_on)
    t1 = min(cmd.t_end, run.rec_t[-1], run.timer_off)

    print(f"\n=== pace lag report =======================================================")
    print(f"file            {rep.file}")
    print(f"workout         {rep.workout!r}   rest policy: {rep.rest_policy}")
    print(f"window          {t0:.0f}s .. {t1:.0f}s ({rep.duration_s:.0f}s), "
          f"{rep.n_transitions} commanded speed changes")

    print("\n--- speed trace vs command ----------------------------------------------")
    print(ascii_plot(run, cmd, t0, t1))

    print("\n--- headline (seconds; lower is better) ---------------------------------")
    print(f"  effective lag              {fmt(m['effective_lag_s'])} s   "
          f"<- IAE / sum|step|, all error sources folded in")
    print(f"    of which transient       {fmt(m['transient_lag_s'])} s   "
          f"<- the actual response delay")
    print(f"  best global shift tau*     {fmt(m['best_shift_s'])} s   "
          f"<- sub-second delay that best aligns the curves")
    print(f"  residual after shifting    {fmt(m['residual_lag_after_shift_s'])} s   "
          f"<- error a pure delay cannot explain")
    print(f"  lag-explained fraction     {fmt(m['lag_explained_fraction'], '{:.1%}')}")

    print("\n--- edge-measured lag (1 Hz records, so quantised to whole seconds) ------")
    print(f"  mean {fmt(m['edge_lag_mean_s'], '{:.2f}')} s   "
          f"median {fmt(m['edge_lag_median_s'], '{:.1f}')} s   "
          f"p90 {fmt(m['edge_lag_p90_s'], '{:.1f}')} s   "
          f"min {fmt(m['edge_lag_min_s'], '{:.1f}')} s   "
          f"max {fmt(m['edge_lag_max_s'], '{:.1f}')} s")
    if m["transitions_missed"]:
        print(f"  !! {m['transitions_missed']} commanded change(s) never showed up "
              f"within {MAX_LAG_SEARCH:.0f}s")

    print("\n--- area between the curves (integral |a-c| dt, metres) ------------------")
    print(f"  IAE total                  {m['iae_m']:8.2f} m   "
          f"({fmt(m['iae_pct_of_distance'], '{:.2f}')}% of the "
          f"{m['commanded_distance_m']:.0f} m commanded)")
    print(f"    transient (responding)   {m['iae_transient_m']:8.2f} m")
    print(f"    dropouts                 {m['iae_dropout_m']:8.2f} m")
    print(f"    steady state (levels)    {m['iae_steady_m']:8.2f} m")
    print(f"  ISE                        {m['ise_m2_s']:8.3f} m^2/s")
    print(f"  signed error               {m['signed_error_m']:+8.2f} m   "
          f"(+ = belt ran ahead of target)")
    print(f"  steady-state gain          {fmt(m['steady_gain'], '{:.5f}')} "
          f"({fmt(m['steady_gain_error_pct'], '{:+.3f}')}%)   "
          f"<- SDM 1/256 quantisation + watch footpod calibration")

    print("\n--- dropouts ------------------------------------------------------------")
    if not rep.dropouts:
        print("  none")
    else:
        print(f"  {m['dropout_count']} event(s), {m['dropout_seconds']:.0f} s total, "
              f"worst depth {m['dropout_worst_depth_pct']:.0f}%")
        for d in rep.dropouts:
            print(f"    t={d['t_start']:6.0f}s  {d['duration_s']:.0f}s  "
                  f"commanded {d['commanded'] * 3.6:.2f} km/h  "
                  f"samples {d['samples']}  "
                  f"distance kept {fmt(d['distance_kept_pct'], '{:.0f}')}%  "
                  f"-> {d['verdict']}")

    print("\n--- ANT telemetry holes -------------------------------------------------")
    if not rep.holes:
        print("  none -- every second of the run carried a usable speed sample")
    else:
        print(f"  {len(rep.holes)} sample(s) with no usable speed while the bridge was "
              f"commanding one:")
        print("    " + ", ".join(f"t={h['t']:.0f}({h['kind']})" for h in rep.holes))
        p = rep.phase
        if p is None:
            print("  (need >=2 holes and a readable firmware/ant_sdm.c to test "
                  "for periodicity)")
        else:
            print(f"  SDM page cycle {p['cycle_s']:.2f}s ({p['cycle_source']}) "
                  f"-- must be the cycle the RECORDING firmware used, or this "
                  f"check is meaningless")
            print(f"  phase within that cycle: {p['phases_s']}")
            print(f"  circular concentration {p['concentration']:.3f} "
                  f"(1.0 = all at the same phase), spread {p['spread_s']:.2f}s")
            if p["clustered"]:
                print(f"  => CLUSTERED at phase {p['mean_phase_s']:.1f}s. These are not "
                      f"random RF loss: the page")
                print(f"     schedule is starving page 1 out of one whole "
                      f"one-second window each cycle.")
            else:
                print("  => not clustered; looks like ordinary RF loss, "
                      "not the page schedule")

    if verbose:
        print("\n--- per-transition ------------------------------------------------------")
        print("      t/s     from      to     edge lag    area")
        for t in rep.transitions:
            print(f"    {t.t:6.0f}  {t.from_v * 3.6:6.2f}  {t.to_v * 3.6:6.2f} km/h    "
                  f"{fmt(t.lag, '{:4.1f}'):>5} s   {t.iae:6.2f} m")
    print()


# ---------------------------------------------------------------------------
# Baseline / regression gate
# ---------------------------------------------------------------------------

#: metric -> (absolute slack, relative slack). A candidate fails when it
#: exceeds baseline + max(abs, rel * baseline). Chosen loose enough that the
#: same firmware re-run passes (1 Hz records quantise the edge lags hard) and
#: tight enough that a real regression trips it.
TOLERANCES = {
    "effective_lag_s": (0.25, 0.15),
    "transient_lag_s": (0.25, 0.15),
    "best_shift_s": (0.25, 0.15),
    "edge_lag_mean_s": (0.35, 0.15),
    "edge_lag_p90_s": (1.0, 0.20),
    "iae_m": (2.0, 0.15),
    "dropout_seconds": (1.0, 0.25),
    "transitions_missed": (0.0, 0.0),
}


def to_json(rep: Report) -> dict:
    return {
        "schema": 1,
        "file": Path(rep.file).name,
        "workout": rep.workout,
        "rest_policy": rep.rest_policy,
        "duration_s": rep.duration_s,
        "n_transitions": rep.n_transitions,
        "metrics": rep.metrics,
        "transitions": [{"t": t.t, "from_kmh": t.from_v * 3.6, "to_kmh": t.to_v * 3.6,
                         "lag_s": t.lag, "iae_m": t.iae} for t in rep.transitions],
        "dropouts": rep.dropouts,
        "speed_holes": rep.holes,
        "hole_phase": rep.phase,
        "tolerances": TOLERANCES,
    }


def compare(rep: Report, baseline: dict) -> bool:
    """Print a regression table. Returns True if everything is within tolerance."""
    base = baseline.get("metrics", {})
    tols = baseline.get("tolerances", TOLERANCES)
    print("--- vs baseline ---------------------------------------------------------")
    print(f"  baseline: {baseline.get('file')} ({baseline.get('rest_policy')} rest policy)")
    if baseline.get("rest_policy") != rep.rest_policy:
        print(f"  !! rest policy differs ({baseline.get('rest_policy')} -> "
              f"{rep.rest_policy}); the reference command timeline is not the same")
    print(f"  {'metric':<28}{'baseline':>10}{'now':>10}{'delta':>10}   verdict")
    ok = True
    for key, (abs_tol, rel_tol) in tols.items():
        b, n = base.get(key), rep.metrics.get(key)
        if b is None or n is None:
            print(f"  {key:<28}{fmt(b, '{:10.3f}', '        --')}"
                  f"{fmt(n, '{:10.3f}', '        --')}{'':>10}   SKIP")
            continue
        limit = b + max(abs_tol, rel_tol * abs(b))
        good = n <= limit
        ok &= good
        flag = "ok" if good else "REGRESSION"
        better = " (improved)" if n < b - 1e-9 else ""
        print(f"  {key:<28}{b:10.3f}{n:10.3f}{n - b:+10.3f}   {flag}{better}")
    print(f"\n  {'PASS' if ok else 'FAIL'}\n")
    return ok


# ---------------------------------------------------------------------------
# Self-test. The scorer is the instrument, so it gets calibrated against
# signals whose answers are known by construction before it is trusted to
# judge firmware. Run with --self-test (no .FIT needed).
# ---------------------------------------------------------------------------


def _synth_run(delay: float, dropout: tuple[float, float] | None = None,
               gain: float = 1.0, n_laps: int = 20, lap_s: float = 10.0) -> Run:
    """A square wave between two speed targets, observed through a pure
    transport delay and a 1 Hz sampler -- i.e. exactly what the watch records.

    mm/s values are chosen to survive the mm/s -> km/h -> m/s chain exactly
    (2500 -> 9.0 km/h -> 2.5 m/s, 3000 -> 10.8 -> 3.0), so any residual error
    the scorer reports is the scorer's, not float noise.
    """
    steps = {
        0: {"target_type": "speed", "custom_target_value_low": 2500,
            "custom_target_value_high": 2500, "intensity": "active"},
        1: {"target_type": "speed", "custom_target_value_low": 3000,
            "custom_target_value_high": 3000, "intensity": "active"},
    }
    laps = [{"t_start": i * lap_s, "t_end": (i + 1) * lap_s,
             "step_index": i % 2, "intensity": "active"} for i in range(n_laps)]
    total = n_laps * lap_s
    truth = Signal([l["t_start"] for l in laps],
                   [2.5 if l["step_index"] == 0 else 3.0 for l in laps], total)

    rec_t, rec_v, rec_d, dist = [], [], [], 0.0
    for k in range(int(total)):
        t = float(k)
        v = truth.at(max(0.0, t - delay))
        v = 0.0 if v is None else v * gain
        dist += v
        if dropout and dropout[0] <= t < dropout[0] + dropout[1]:
            rec_v.append(0.0)          # speed sample lost...
            dist += v                  # ...but distance kept advancing
        else:
            rec_v.append(v)
        rec_t.append(t)
        rec_d.append(dist)
    return Run("synthetic", None, rec_t, rec_v, rec_d, steps, laps, 0.0, total)


def self_test() -> int:
    fails = []

    def check(name, got, want, tol):
        ok = got is not None and abs(got - want) <= tol
        print(f"  {'ok  ' if ok else 'FAIL'}  {name:<34} "
              f"got {fmt(got, '{:.3f}'):>8}  want {want:.3f} +-{tol}")
        if not ok:
            fails.append(name)

    print("perfect tracking, zero delay")
    m = analyse(_synth_run(0.0), None, "hold", Path("<synthetic>")).metrics
    check("effective_lag_s", m["effective_lag_s"], 0.0, 0.001)
    check("edge_lag_mean_s", m["edge_lag_mean_s"], 0.0, 0.001)
    check("best_shift_s", m["best_shift_s"], 0.0, 0.001)
    check("iae_m", m["iae_m"], 0.0, 0.001)
    check("dropout_count", m["dropout_count"], 0, 0)

    # A 2.0 s delay sampled at 1 Hz with step boundaries on integer seconds is
    # indistinguishable from any delay in (1, 2]: every estimator must land on
    # the sampled edge, 2.0. That equality is the point -- three independent
    # estimators agreeing is what makes a change in any of them meaningful.
    print("\npure 2.0 s transport delay")
    m = analyse(_synth_run(2.0), None, "hold", Path("<synthetic>")).metrics
    check("effective_lag_s", m["effective_lag_s"], 2.0, 0.01)
    check("transient_lag_s", m["transient_lag_s"], 2.0, 0.01)
    check("edge_lag_mean_s", m["edge_lag_mean_s"], 2.0, 0.01)
    check("best_shift_s", m["best_shift_s"], 2.0, 0.03)
    check("residual_after_shift", m["residual_lag_after_shift_s"], 0.0, 0.03)
    check("lag_explained_fraction", m["lag_explained_fraction"], 1.0, 0.02)
    # IAE = sum over transitions of |step| * delay = 19 * 0.5 m/s * 2 s
    check("iae_m", m["iae_m"], 19 * 0.5 * 2.0, 0.05)

    print("\n1.0 s delay + a 3 s speed dropout at t=45 (distance kept advancing)")
    m = analyse(_synth_run(1.0, dropout=(45.0, 3.0)), None, "hold",
                Path("<synthetic>")).metrics
    check("edge_lag_mean_s", m["edge_lag_mean_s"], 1.0, 0.01)
    check("dropout_count", m["dropout_count"], 1, 0)
    check("dropout_seconds", m["dropout_seconds"], 3.0, 0.001)
    # t=45..47 falls in lap 4, which is step 0 -> 2.5 m/s commanded.
    check("iae_dropout_m", m["iae_dropout_m"], 3.0 * 2.5, 0.01)
    check("transient_lag_s", m["transient_lag_s"], 1.0, 0.02)    # unpolluted

    rep = analyse(_synth_run(1.0, dropout=(45.0, 3.0)), None, "hold",
                  Path("<synthetic>"))
    verdict = rep.dropouts[0]["verdict"]
    ok = "telemetry glitch" in verdict
    print(f"  {'ok  ' if ok else 'FAIL'}  {'dropout classified':<34} {verdict}")
    if not ok:
        fails.append("dropout verdict")

    # A level error is not a delay, but it does put area between the curves, so
    # it has to show up in the headline number too -- exactly, and in the steady
    # bucket rather than the transient one. 10 laps at 2.5 m/s + 10 at 3.0 over
    # 10 s each = 550 m commanded, so a 1% gain error is 5.50 m of area, over
    # sum|step| = 19 * 0.5 m/s.
    print("\nsteady-state gain error of -1% (a level error, not a delay)")
    m = analyse(_synth_run(0.0, gain=0.99), None, "hold", Path("<synthetic>")).metrics
    check("steady_gain", m["steady_gain"], 0.99, 0.001)
    check("commanded_distance_m", m["commanded_distance_m"], 550.0, 0.01)
    check("iae_m", m["iae_m"], 5.50, 0.01)
    check("effective_lag_s", m["effective_lag_s"], 5.50 / 9.5, 0.005)
    # The buckets split it, and the split is exact rather than approximate: a
    # zero-lag transition still opens a 1 s window, which scoops up one second
    # of the (ever-present) level error each time. 10 transitions land on
    # 3.0 m/s and 9 on 2.5, so transient = 0.01 * (10*3.0 + 9*2.5) = 0.525 m and
    # steady keeps the remaining 4.975. The point of the check is that a pure
    # level error stays overwhelmingly in the steady bucket and cannot pass
    # itself off as a response delay.
    check("iae_transient_m", m["iae_transient_m"], 0.525, 0.01)
    check("iae_steady_m", m["iae_steady_m"], 4.975, 0.01)
    check("transient_lag_s", m["transient_lag_s"], 0.525 / 9.5, 0.005)

    print("\nrest-step policy: 'walk' drops to REST_SPEED_KMH, 'hold' does not")
    rest_step = {"target_type": "open", "intensity": "rest"}
    kind, mps = decode_action(rest_step, firmware_rest_kmh())
    check("walk rest speed (km/h)", mps * 3.6, firmware_rest_kmh(), 0.001)
    kind2, _ = decode_action(rest_step, None)
    ok = kind == "speed" and kind2 == "none"
    print(f"  {'ok  ' if ok else 'FAIL'}  {'hold rest -> ACT_NONE':<34} {kind2}")
    if not ok:
        fails.append("rest policy")

    print(f"\nself-test: {'PASS' if not fails else 'FAIL ' + ', '.join(fails)}")
    return 0 if not fails else 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Score treadmill pace tracking and response lag from a .FIT.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("--- what it measures")[0])
    ap.add_argument("fit", type=Path, nargs="?",
                    help="activity .FIT recorded in SDM:TGT mode")
    ap.add_argument("--self-test", action="store_true",
                    help="check the scorer against synthetic signals with known "
                         "answers; needs no .FIT")
    ap.add_argument("--rest-policy", choices=("auto", "walk", "hold"), default="auto",
                    help="how a rest step with no speed target is commanded: "
                         "'walk' = current firmware (REST_SPEED_KMH), "
                         "'hold' = pre-4932511 firmware, 'auto' = whichever fits "
                         "the trace (default)")
    ap.add_argument("--json", type=Path, help="write the full report as JSON")
    ap.add_argument("--write-baseline", type=Path,
                    help="write this run as the reference for future comparisons")
    ap.add_argument("--baseline", type=Path,
                    help="compare against a stored baseline; non-zero exit on regression")
    ap.add_argument("--sdm-cycle-s", type=float, default=None,
                    help="length of the ANT SDM page-rotation cycle, for the "
                         "hole-periodicity check. Defaults to whatever "
                         "firmware/ant_sdm.c currently says; pass the value the "
                         "*recording* firmware used when scoring an old trace "
                         "(17.0 for anything before the page-spread change).")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="also print the per-transition table")
    ap.add_argument("-q", "--quiet", action="store_true", help="only the summary line")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.fit is None:
        ap.error("a .FIT file is required (or use --self-test)")
    if not args.fit.exists():
        return print(f"no such file: {args.fit}", file=sys.stderr) or 2

    run = load(args.fit)
    rest_kmh = firmware_rest_kmh()
    if args.sdm_cycle_s:
        cycle_s, cycle_src = args.sdm_cycle_s, "--sdm-cycle-s"
    else:
        cycle_s, cycle_src = firmware_sdm_cycle_s(), "from firmware/ant_sdm.c HEAD"

    candidates = {"walk": rest_kmh, "hold": None}
    if args.rest_policy == "auto":
        scored = {name: analyse(run, kmh, name, args.fit, cycle_s, cycle_src)
                  for name, kmh in candidates.items()}
        policy = min(scored, key=lambda k: scored[k].metrics["iae_m"])
        rep = scored[policy]
        other = "hold" if policy == "walk" else "walk"
        ratio = scored[other].metrics["iae_m"] / max(rep.metrics["iae_m"], 1e-9)
        print(f"rest policy: detected {policy!r} "
              f"({'REST_SPEED_KMH=%.1f' % rest_kmh if policy == 'walk' else 'hold the work pace'}"
              f"); the {other!r} model fits {ratio:.1f}x worse")
    else:
        policy = args.rest_policy
        rep = analyse(run, candidates[policy], policy, args.fit, cycle_s, cycle_src)

    cmd, _ = command_signal(run, candidates[policy])
    if not args.quiet:
        print_report(rep, run, cmd, args.verbose)

    m = rep.metrics
    print(f"SCORE  effective_lag={m['effective_lag_s']:.3f}s  "
          f"edge_lag_mean={fmt(m['edge_lag_mean_s'], '{:.2f}')}s  "
          f"IAE={m['iae_m']:.2f}m  dropouts={m['dropout_count']}")

    payload = to_json(rep)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"wrote {args.json}")
    if args.write_baseline:
        args.write_baseline.parent.mkdir(parents=True, exist_ok=True)
        args.write_baseline.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"wrote baseline {args.write_baseline}")
    if args.baseline:
        print()
        if not compare(rep, json.loads(args.baseline.read_text())):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
