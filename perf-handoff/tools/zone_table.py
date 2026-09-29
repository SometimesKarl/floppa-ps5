"""Tracy zones by self time per frame, with inclusive time, calls per frame and self us per call.

Usage: zone_table.py RUN_DIR [FRAMES] [TOP=45]
Reads tracy-zones-self.csv and tracy-zones.csv (tracy-csvexport with and without -e). FRAMES
defaults to the number of Presenter::Present calls in the inclusive table. Wait zones are skipped.
"""
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 45


def load(name):
    out = {}
    with open(run / name, newline="") as f:
        for r in csv.DictReader(f):
            src = r["src_file"].replace("\\", "/").split("/")[-1] + ":" + r["src_line"]
            out[(r["name"], src)] = (int(r["total_ns"]), int(r["counts"]))
    return out


self_t = load("tracy-zones-self.csv")
incl_t = load("tracy-zones.csv")
frames = int(sys.argv[2]) if len(sys.argv) > 2 else next(
    c for (n, _), (_, c) in incl_t.items() if n.endswith("Presenter::Present") or n.endswith("Presenter::Impl::Present"))
skip = ("PthreadCondWait", "KernelWaitSema", "KernelWaitEqueue", "MasterSemaphore::Wait",
        "CommandProcessor::Process", "VkCtx::Collect", "ReadMemoryAsync")
print(f"frames {frames}")
print(f"{'self ms/f':>9} {'incl ms/f':>9} {'calls/f':>8} {'self us/call':>12}  zone")
rows = sorted(self_t.items(), key=lambda kv: -kv[1][0])
shown = 0
for (name, src), (t, c) in rows:
    if any(s in name for s in skip):
        continue
    inc = incl_t.get((name, src), (0, 0))[0]
    print(f"{t / 1e6 / frames:9.3f} {inc / 1e6 / frames:9.3f} {c / frames:8.1f} {t / 1e3 / max(c, 1):12.2f}  "
          f"{name[:62]} ({src})")
    shown += 1
    if shown >= top:
        break
