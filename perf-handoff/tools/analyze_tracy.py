"""Summarize a Tracy capture exported by tracy-csvexport (run_experiment.py --tracy-seconds).

Usage: python analyze_tracy.py RUN_DIR
Reads tracy-events.csv (-u: one row per zone event) and tracy-zones*.csv (stats).
Frames are delimited by successive starts of Presenter::Present (FrameMark is not
exported to CSV). Tracy exports a thread *index*, not an OS id; thread roles are
inferred from the zones they run (e.g. CommandProcessor::Process -> guest GPU).

Outputs RUN_DIR/tracy-analysis.json and prints:
  - frame-interval statistics from Present starts,
  - per thread index: role hints and time per frame in each zone (inclusive),
  - for each long frame (> 300 ms): the longest zones overlapping it.
"""
import csv
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

run = Path(sys.argv[1])
rows = []
with open(run / "tracy-events.csv", newline="") as f:
    for r in csv.DictReader(f):
        try:
            rows.append((r["name"], int(r["src_line"]), int(r["ns_since_start"]), int(r["exec_time_ns"]),
                         int(r["thread"]), r["src_file"].replace("\\", "/").split("/")[-1]))
        except (KeyError, ValueError):
            continue

present = sorted(s for n, _, s, _, _, _ in rows if n.endswith("Presenter::Present") or n == "Present")
if not present:
    present = sorted(s for n, _, s, _, _, fl in rows if "Present" in n and fl == "swapchain.cpp")
intervals = [(b - a) / 1e6 for a, b in zip(present, present[1:])]
span_ns = present[-1] - present[0] if len(present) > 1 else max(s + d for _, _, s, d, _, _ in rows) - min(s for _, _, s, _, _, _ in rows)
frames = max(len(intervals), 1)

by_thread = defaultdict(lambda: defaultdict(lambda: [0, 0]))  # thread -> zone -> [ns, count]
for name, line, start, dur, thr, fl in rows:
    key = f"{name} ({fl}:{line})"
    by_thread[thr][key][0] += dur
    by_thread[thr][key][1] += 1

ROLE_HINTS = [("CommandProcessor::Process", "guest GPU command processor (Thread_Gpu)"),
              ("Presenter::Present", "presenter"), ("UpdateTitle", "presenter (title)"),
              ("PthreadCond", "guest thread"), ("KernelWaitEqueue", "guest thread"),
              ("KernelWaitSema", "guest thread"), ("KernelWaitEventFlag", "guest thread"),
              ("VideoOutWaitVblank", "guest thread (vblank waiter)")]
threads = {}
for thr, zones in by_thread.items():
    roles = sorted({role for hint, role in ROLE_HINTS for z in zones if hint in z})
    top = sorted(zones.items(), key=lambda kv: kv[1][0], reverse=True)[:12]
    threads[thr] = dict(roles=roles, zones_ms_per_frame={k: round(v[0] / 1e6 / frames, 3) for k, v in top},
                        zone_counts_per_frame={k: round(v[1] / frames, 2) for k, v in top})

long_frames = []
for a, b in zip(present, present[1:]):
    if (b - a) / 1e6 > 300:
        overlapping = [(n, thr, s, d) for n, _, s, d, thr, _ in rows if s < b and s + d > a and d > 50e6]
        overlapping.sort(key=lambda x: x[3], reverse=True)
        long_frames.append(dict(start_ms=round((a - present[0]) / 1e6, 1), length_ms=round((b - a) / 1e6, 1),
                                zones=[dict(zone=n, thread=t, ms=round(d / 1e6, 1)) for n, t, s, d in overlapping[:10]]))

out = dict(
    present_count=len(present), span_s=span_ns / 1e9,
    fps=(len(present) - 1) / (span_ns / 1e9) if len(present) > 1 else None,
    frame_ms_median=statistics.median(intervals) if intervals else None,
    frame_ms_p95=sorted(intervals)[int(0.95 * (len(intervals) - 1))] if intervals else None,
    frame_ms_max=max(intervals) if intervals else None,
    threads=threads, long_frames=long_frames,
)
(run / "tracy-analysis.json").write_text(json.dumps(out, indent=2))
print(f"presents={out['present_count']} span={out['span_s']:.1f}s fps={out['fps']} "
      f"median={out['frame_ms_median']} p95={out['frame_ms_p95']} max={out['frame_ms_max']}")
for thr, t in sorted(threads.items(), key=lambda kv: -sum(kv[1]["zones_ms_per_frame"].values())):
    print(f"-- thread {thr} {t['roles']}")
    for z, ms in list(t["zones_ms_per_frame"].items())[:8]:
        print(f"     {ms:9.3f} ms/frame  x{t['zone_counts_per_frame'][z]:<8} {z}")
for lf in long_frames:
    print(f"LONG FRAME at {lf['start_ms']} ms: {lf['length_ms']} ms")
    for z in lf["zones"][:6]:
        print(f"     thread {z['thread']:3d} {z['ms']:9.1f} ms  {z['zone']}")
