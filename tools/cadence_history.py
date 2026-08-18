#!/usr/bin/env -S uv run --quiet --script
# /// script
# requires-python = ">=3.10"
# dependencies = ["garmin-fit-sdk>=21.141"]
# ///
"""Cadence signature of every recording, oldest first — one row per .FIT.

Built to answer "when did the cadence problem start, and did any of the fixes
move it?" from the recordings themselves rather than from the changelog. Point
it at every .FIT you have and read the table against `git log` on
firmware/ant_sdm.c:

    tools/cadence_history.py ~/Downloads/*_ACTIVITY.fit logs/compare/*.fit

The columns that matter are `ant+ sensors` — `development/stride_speed_distance`
is THIS bridge, anything else is a real sensor — and `cad=0`, the number of
records in which the watch wrote a cadence of exactly zero. A session
`avg_running_cadence` in the 230-253 range with `max` 0 is the failure
signature; it is not a plausible cadence, and the `max` of 0 is what gives it
away as an accumulator fed mostly zeros rather than a real measurement.
"""
import sys, collections
from garmin_fit_sdk import Decoder, Stream
rows=[]
for p in sys.argv[1:]:
    try:
        msgs,_=Decoder(Stream.from_file(p)).read()
    except Exception as e:
        print("skip",p,e); continue
    s=(msgs.get("session_mesgs") or [{}])[0]
    fid=(msgs.get("file_id_mesgs") or [{}])[0]
    sport=s.get("sport")
    ants=[]
    for m in msgs.get("device_info_mesgs",[]):
        if m.get("source_type")=="antplus":
            mfg=m.get("manufacturer"); typ=m.get("antplus_device_type")
            tag="%s/%s"%(mfg,typ)
            if tag not in ants: ants.append(tag)
    recs=msgs.get("record_mesgs",[])
    cad=[r.get("cadence") for r in recs if r.get("cadence") is not None]
    c=collections.Counter(cad)
    nz=[v for v in cad if v]
    rows.append((fid.get("time_created"), p.split("/")[-1], fid.get("garmin_product"),
                 sport, s.get("avg_running_cadence"), s.get("max_running_cadence"),
                 s.get("total_strides"), len(cad), c.get(0,0),
                 (min(nz),max(nz)) if nz else None, ants))
rows.sort(key=lambda r:(r[0] is None, r[0]))
print("%-19s %-30s %-18s %-9s %5s %5s %7s %6s %6s %-12s %s"%(
 "created","file","watch","sport","avg","max","strides","recs","cad=0","nonzero","ant+ sensors"))
for r in rows:
    print("%-19s %-30s %-18s %-9s %5s %5s %7s %6d %6d %-12s %s"%(
      str(r[0])[:19], r[1][:30], str(r[2])[:18], str(r[3])[:9], r[4], r[5], r[6],
      r[7], r[8], str(r[9]), ",".join(r[10]) or "-"))
