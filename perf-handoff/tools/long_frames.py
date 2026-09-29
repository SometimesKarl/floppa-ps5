"""Which CPU zones grow in long frames? Compares zone time per frame in long vs normal frames.

Usage: long_frames.py RUN_DIR [LONG_MS]
Frames are delimited by Presenter::Present zones in tracy-events.csv (tracy-csvexport -u).
"""
import bisect
import collections
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
long_ms = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
events = []
presents = []
with open(run / "tracy-events.csv") as f:
    for r in csv.DictReader(f):
        start = int(r["ns_since_start"])
        dur = int(r["exec_time_ns"])
        name = r["name"]
        if name == "Present" or name.endswith("Presenter::Present"):
            presents.append(start)
        events.append((start, dur, name, r["thread"]))
presents.sort()
if len(presents) < 3:
    raise SystemExit("not enough Present zones")
intervals = [(presents[i], presents[i + 1]) for i in range(len(presents) - 1)]
long_idx = {i for i, (a, b) in enumerate(intervals) if (b - a) / 1e6 >= long_ms}
print(f"frames {len(intervals)}, long (>= {long_ms} ms) {len(long_idx)}")

# Attribute each zone's duration to the frame interval containing its start (self time is not
# available here; nested zones double count, so compare like with like between the groups).
per = {True: collections.defaultdict(int), False: collections.defaultdict(int)}
starts = [a for a, _ in intervals]
for start, dur, name, thread in events:
    i = bisect.bisect_right(starts, start) - 1
    if i < 0 or i >= len(intervals) or start >= intervals[i][1]:
        continue
    per[i in long_idx][(thread, name)] += dur
n_long = max(len(long_idx), 1)
n_norm = max(len(intervals) - len(long_idx), 1)
rows = []
for key in set(per[True]) | set(per[False]):
    lt = per[True].get(key, 0) / n_long / 1e6
    nt = per[False].get(key, 0) / n_norm / 1e6
    rows.append((lt - nt, lt, nt, key))
rows.sort(reverse=True)
print("zone (thread)                                              long ms/frame  normal ms/frame  delta")
for delta, lt, nt, (thread, name) in rows[:25]:
    if delta <= 0.05:
        break
    print(f"  {name[:52]:52s} t{thread:>3s} {lt:13.2f} {nt:16.2f} {delta:+6.2f}")
