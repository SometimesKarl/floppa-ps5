"""Attribute Tracy zone time to call paths, per thread (streams tracy-events.csv).

Usage: python tracy_nesting.py RUN_DIR [min_us=50]
Keeps events >= min_us (an ancestor always lasts at least as long as its
child, so nesting among kept events is exact), rebuilds per-thread stacks, and
reports for each thread: wall time covered by top-level zones, and inclusive
time of wait zones grouped by their ancestor chain.
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

run = Path(sys.argv[1])
min_ns = int(float(sys.argv[2]) * 1000) if len(sys.argv) > 2 else 50_000
WAITS = ("MasterSemaphore::Wait", "CommandScheduler::Wait", "PthreadCondWait", "KernelWaitSema",
         "KernelWaitEqueue", "KernelWaitEventFlag", "PthreadCondTimedwait", "VideoOutWaitVblank",
         "UpdateTitle", "CommandScheduler::Finish", "FlushAndWait")

events = defaultdict(list)
t_min, t_max = None, 0
with open(run / "tracy-events.csv", newline="") as f:
    for r in csv.DictReader(f):
        d = int(r["exec_time_ns"])
        s = int(r["ns_since_start"])
        t_min = s if t_min is None else min(t_min, s)
        t_max = max(t_max, s + d)
        if d >= min_ns:
            events[int(r["thread"])].append((s, -d, r["name"].split("::")[-1] if "Libs::" in r["name"] else r["name"]))
span = (t_max - t_min) / 1e9
presents = 0
report = []
for thr, ev in events.items():
    ev.sort()
    stack = []
    top_ns = 0
    by_path = defaultdict(lambda: [0, 0])
    self_like = defaultdict(int)
    for s, negd, name in ev:
        d = -negd
        e = s + d
        while stack and stack[-1][1] <= s:
            stack.pop()
        if not stack:
            top_ns += d
        if name.endswith("Present") and "Presenter" in name or name == "Present":
            presents += 1
        if any(w in name for w in WAITS):
            # attribute only the outermost wait on this path
            if not any(any(w in n for w in WAITS) for _, _, n in stack):
                path = " > ".join(n for _, _, n in stack[-3:]) or "(top)"
                by_path[f"{path} > {name}"][0] += d
                by_path[f"{path} > {name}"][1] += 1
        self_like[name] += d
        stack.append((s, e, name))
    wait_ns = sum(v[0] for v in by_path.values())
    report.append((top_ns - wait_ns, top_ns, thr, by_path, self_like))

report.sort(reverse=True)
print(f"trace span {span:.2f}s, Present zones seen (>= min) {presents}")
for busy_ns, top_ns, thr, by_path, self_like in report[:10]:
    print(f"\n== thread {thr}: non-wait zone time {busy_ns / 1e9:.2f}s; zones cover {top_ns / 1e9:.2f}s of {span:.2f}s")
    names = sorted(self_like.items(), key=lambda kv: -kv[1])[:6]
    print("   biggest zones (inclusive):", ", ".join(f"{n} {v / 1e9:.2f}s" for n, v in names))
    for path, (ns, cnt) in sorted(by_path.items(), key=lambda kv: -kv[1][0])[:8]:
        print(f"   wait {ns / 1e9:7.2f}s x{cnt:<6} {path}")
