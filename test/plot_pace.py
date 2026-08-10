#!/usr/bin/env python3
"""Plot the commanded vs recorded speed trace and per-transition lag from a .FIT.

Reuses the parser/policy mirror from pace_lag_report.py, so the picture can
never disagree with the score.

Usage:
    uv run --with garmin-fit-sdk --with matplotlib python3 test/plot_pace.py \
        /path/to/run.fit [--baseline test/baselines/x.json] [-o out.png]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pace_lag_report as plr  # noqa: E402

DT = 0.25  # sampling grid for the step signals, s


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("fit", type=Path, help="activity .FIT recorded in SDM:TGT mode")
    ap.add_argument("--baseline", type=Path, default=None,
                    help="baseline JSON to overlay its mean edge lag")
    ap.add_argument("-o", "--out", type=Path, default=None,
                    help="output PNG (default: <fit>.png next to the input)")
    ap.add_argument("--sdm-cycle-s", type=float, default=None,
                    help="ANT page-cycle length of the *recording* firmware "
                         "(see pace_lag_report.py --sdm-cycle-s)")
    args = ap.parse_args()

    run = plr.load(args.fit)
    rest_kmh = plr.firmware_rest_kmh()
    if args.sdm_cycle_s:
        cycle_s, cycle_src = args.sdm_cycle_s, "--sdm-cycle-s"
    else:
        cycle_s, cycle_src = plr.firmware_sdm_cycle_s(), "firmware/ant_sdm.c HEAD"

    # Same rest-policy auto-detection as the scorer's main().
    scored = {name: plr.analyse(run, kmh, name, args.fit, cycle_s, cycle_src)
              for name, kmh in (("walk", rest_kmh), ("hold", None))}
    policy = min(scored, key=lambda k: scored[k].metrics["iae_m"])
    rep = scored[policy]
    cmd, _ = plr.command_signal(run, rest_kmh if policy == "walk" else None)
    m = rep.metrics

    duration = run.rec_t[-1] - run.rec_t[0]
    rec_sig = plr.Signal(run.rec_t, run.rec_v, run.rec_t[-1])
    t = [i * DT for i in range(int(duration / DT) + 1)]
    rec = [rec_sig.at(tt) for tt in t]
    com = [cmd.at(tt) for tt in t]

    fig, (ax1, ax2) = plt.subplots(
        2, 1, figsize=(13, 9), sharex=True,
        gridspec_kw={"height_ratios": [3, 1], "hspace": 0.08})

    # ---- panel 1: trace -------------------------------------------------
    ax1.step(t, com, where="post", color="#1f5fa8", lw=1.2, label="commanded")
    ax1.step(t, rec, where="post", color="#e07b1a", lw=1.0, label="recorded")
    ax1.set_ylabel("speed (km/h)")

    # command transitions
    for tr in rep.transitions:
        ax1.axvline(tr.t, color="#bbbbbb", lw=0.6, ls=":", zorder=0)

    # dropout windows, shaded by verdict
    for d in rep.dropouts:
        lo, hi = d["t_start"], d["t_end"] + 1.0
        bad = "genuinely" in d["verdict"]
        ax1.axvspan(lo, hi, color="#d64545" if bad else "#e8c15a",
                    alpha=0.18, zorder=0)
        label = "belt genuinely slowed" if bad else "telemetry glitch"
        ax1.text(lo + 0.2, ax1.get_ylim()[1] * 0.97, label, fontsize=8,
                 color="#8a2b2b" if bad else "#8a7420", va="top")

    # ANT telemetry holes
    for h in rep.holes:
        if h["kind"] == "zero":
            ax1.plot(h["t"], 0.0, "x", color="#d64545", ms=6, mew=1.5)
        else:
            c = cmd.at(h["t"])
            if c is not None:
                ax1.plot(h["t"], c, "v", color="#d64545", ms=5)

    # workout start / rest region shading
    ax1.axvline(0.0, color="k", lw=0.8)

    legend_handles = [
        Line2D([], [], color="#1f5fa8", lw=1.2, label="commanded target"),
        Line2D([], [], color="#e07b1a", lw=1.0, label="recorded (SDM:TGT)"),
        Line2D([], [], color="#d64545", marker="x", ls="", ms=6,
               label="speed hole (0 / missing)"),
        Line2D([], [], color="#d64545", alpha=0.35, lw=6, label="belt genuinely slowed"),
        Line2D([], [], color="#e8c15a", alpha=0.35, lw=6, label="telemetry glitch"),
    ]
    ax1.legend(handles=legend_handles, loc="upper right", fontsize=9, ncol=2)
    ax1.set_xlim(0, duration)

    # ---- panel 2: per-transition edge lag ------------------------------
    ts = [tr.t for tr in rep.transitions]
    lags = [tr.lag if tr.lag is not None else 0.0 for tr in rep.transitions]
    colors = ["#d64545" if (l is not None and l >= 5) else "#1f5fa8"
              for l in (tr.lag for tr in rep.transitions)]
    ax2.bar(ts, lags, width=4.0, color=colors, alpha=0.85)
    ax2.set_ylabel("edge lag (s)")
    ax2.set_xlabel("time (s)")
    ax2.set_ylim(0, max(lags + [1.0]) * 1.25)
    ax2.axhline(m["edge_lag_mean_s"], color="#e07b1a", ls="--", lw=1.2,
                label=f"this run mean {m['edge_lag_mean_s']:.2f} s")
    if args.baseline:
        b = json.loads(args.baseline.read_text())
        if b.get("metrics", {}).get("edge_lag_mean_s") is not None:
            bm = b["metrics"]["edge_lag_mean_s"]
            ax2.axhline(bm, color="#555555", ls=":", lw=1.2,
                        label=f"baseline mean {bm:.2f} s")
    ax2.legend(fontsize=9, loc="upper left")

    title = (f"{args.fit.name}  —  {rep.workout}  ({policy} rest policy)\n"
             f"effective lag {m['effective_lag_s']:.3f} s   "
             f"edge lag mean {m['edge_lag_mean_s']:.2f} s / max "
             f"{m['edge_lag_max_s']:.1f} s   "
             f"IAE {m['iae_m']:.1f} m   "
             f"dropouts {m['dropout_count']} ({m['dropout_seconds']:.0f} s)")
    fig.suptitle(title, fontsize=11, y=0.995)

    out = args.out or args.fit.with_name(args.fit.name + ".pace.png")
    fig.savefig(out, dpi=150, bbox_inches="tight")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
