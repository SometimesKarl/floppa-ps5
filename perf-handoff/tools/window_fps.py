"""Frame statistics for a time window at the end of a frame log.

Usage: window_fps.py FRAMES_CSV [SKIP_END_S=6] [LENGTH_S=40]
Uses game-frame presentations (kinds 0/1) whose present_end falls in
[last - SKIP_END_S - LENGTH_S, last - SKIP_END_S].
"""
import csv
import sys

path = sys.argv[1]
skip = float(sys.argv[2]) if len(sys.argv) > 2 else 6
length = float(sys.argv[3]) if len(sys.argv) > 3 else 40
freq = 10_000_000
rows = []
for r in csv.reader(open(path)):
    if not r:
        continue
    if r[0].startswith("#"):
        if "qpc_frequency=" in r[0]:
            freq = int(r[0].split("=")[1])
        continue
    if r[0] in ("0", "1"):
        rows.append(int(r[7]))
end = rows[-1] - skip * freq
begin = end - length * freq
t = [q for q in rows if begin <= q <= end]
iv = sorted((b - a) * 1000 / freq for a, b in zip(t, t[1:]))
n = len(iv)
if n < 10:
    sys.exit("too few frames")
mean = sum(iv) / n
low1 = sum(iv[-max(1, n // 100):]) / max(1, n // 100)
low01 = iv[-1]
print(f"{n} frames: avg {1000 / mean:.1f} FPS | frame p50 {iv[n // 2]:.1f} ms, p90 {iv[int(n * .9)]:.1f}, "
      f"p99 {iv[int(n * .99)]:.1f} | 1% low {1000 / low1:.1f} FPS, worst {low01:.0f} ms")
