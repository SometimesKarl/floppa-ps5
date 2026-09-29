"""GPU busy/idle breakdown from a Tracy GPU-zone export (tracy-gpu.csv, tracy-csvexport -g).

Usage: gpu_timeline.py RUN_DIR [FRAMES]
FRAMES defaults to the number of Present zones in tracy-events.csv, when present.
"""
import collections
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
rows = list(csv.DictReader(open(run / "tracy-gpu.csv")))
zones = [(int(r["Time from start of program"]), int(r["GPU execution time"]), r["name"]) for r in rows]
zones.sort()
if not zones:
    raise SystemExit("no GPU zones")
t0 = zones[0][0]
t1 = max(s + d for s, d, _ in zones)
span_ms = (t1 - t0) / 1e6

frames = int(sys.argv[2]) if len(sys.argv) > 2 else 0
if not frames and (run / "tracy-events.csv").exists():
    with open(run / "tracy-events.csv") as f:
        for r in csv.DictReader(f):
            # CPU and GPU zone clocks use different bases in the export; count every Present.
            if r["name"] == "Present" or r["name"].endswith("Presenter::Present"):
                frames += 1
frames = max(frames, 1)

# Union of zone intervals = time the GPU executed at least one traced command.
busy = 0
gaps = []
cur_s, cur_e = zones[0][0], zones[0][0] + zones[0][1]
for s, d, _ in zones[1:]:
    e = s + d
    if s > cur_e:
        busy += cur_e - cur_s
        gaps.append(s - cur_e)
        cur_s, cur_e = s, e
    else:
        cur_e = max(cur_e, e)
busy += cur_e - cur_s

by_name = collections.defaultdict(lambda: [0, 0])
for _, d, n in zones:
    by_name[n][0] += 1
    by_name[n][1] += d

frame_ms = span_ms / frames
print(f"span {span_ms:.0f} ms, frames {frames}, frame {frame_ms:.2f} ms")
print(f"GPU busy (zone union) {busy / 1e6 / frames:.2f} ms/frame = {100 * busy / 1e6 / span_ms:.1f}% of time")
buckets = [(0, 10_000), (10_000, 100_000), (100_000, 1_000_000), (1_000_000, 5_000_000), (5_000_000, 10**12)]
for lo, hi in buckets:
    sel = [g for g in gaps if lo <= g < hi]
    print(f"  idle gaps {lo / 1e3:>7.0f}-{hi / 1e3:<9.0f} us: {len(sel) / frames:7.1f}/frame, "
          f"{sum(sel) / 1e6 / frames:6.2f} ms/frame")
print("zones (count/frame, summed ms/frame; zones overlap, so sums exceed busy time):")
for n, (c, d) in sorted(by_name.items(), key=lambda kv: -kv[1][1])[:20]:
    print(f"  {n[:44]:44s} {c / frames:7.1f} {d / 1e6 / frames:7.2f}")
