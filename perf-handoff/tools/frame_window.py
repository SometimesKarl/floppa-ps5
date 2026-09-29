"""Frame-time statistics between two frame ids of a KYTY_FRAME_LOG csv (game frames, kinds 0/1).

Usage: frame_window.py FRAMES_CSV FIRST_ID LAST_ID
Prints average FPS, p50/p90/p95/p99 frame time, 1% low (1000 / mean of the slowest 1% of frame
times) and the worst frame, from the presentation end times.
"""
import csv
import sys

path, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
freq = 10_000_000
ends = []
with open(path) as f:
    for row in csv.DictReader(line for line in f if not line.startswith("#")):
        if row["kind"] in ("0", "1") and first <= int(row["id"]) <= last:
            ends.append(int(row["present_end_qpc"]))
ends.sort()
dts = sorted((b - a) * 1000 / freq for a, b in zip(ends, ends[1:]))
if not dts:
    sys.exit("no frames in range")
q = lambda p: dts[min(len(dts) - 1, int(p * len(dts)))]
slow = dts[-max(1, len(dts) // 100):]
span = (ends[-1] - ends[0]) / freq
print(f"frames {len(dts)} over {span:.1f} s: avg {len(dts) / span:.1f} FPS | p50 {q(.5):.1f} p90 {q(.9):.1f} "
      f"p95 {q(.95):.1f} p99 {q(.99):.1f} ms | 1% low {1000 / (sum(slow) / len(slow)):.1f} FPS | "
      f"worst {dts[-1]:.0f} ms | frames >50 ms: {sum(d > 50 for d in dts)}, >100 ms: {sum(d > 100 for d in dts)}")
