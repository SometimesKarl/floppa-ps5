"""Game frames per 10 s window of a KYTY_FRAME_LOG csv, aligned to drive.py's keys.log clock.

Usage: windows10.py RUN_DIR [WINDOW_S=10]
Each line: window start (seconds since the first logged frame), frames in the window, FPS, worst
frame time (ms). keys.log times are seconds since launch, so the first key line is printed with the
frame clock's value of the same moment only approximately (launch-to-first-frame offset unknown);
windows are meant for comparing runs driven by the same route.
"""
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
window = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
freq = 10_000_000
ends = []
with open(run / "frames-emu.csv") as f:
    for row in csv.DictReader(line for line in f if not line.startswith("#")):
        if row["kind"] in ("0", "1"):
            ends.append(int(row["present_end_qpc"]))
ends.sort()
if not ends:
    sys.exit("no game frames")
t0 = ends[0]
buckets = {}
prev = None
for e in ends:
    b = int((e - t0) / freq // window)
    n, worst = buckets.get(b, (0, 0.0))
    dt = (e - prev) * 1000 / freq if prev is not None else 0.0
    buckets[b] = (n + 1, max(worst, dt))
    prev = e
for b in sorted(buckets):
    n, worst = buckets[b]
    print(f"{b * window:6.0f}s {n:5d} frames {n / window:5.1f} FPS  worst {worst:6.1f} ms")
