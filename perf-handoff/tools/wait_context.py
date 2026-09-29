"""Which sibling zones precede each wait inside a parent zone, on one Tracy thread.

Usage: wait_context.py RUN_DIR [PARENT="GuestGpu::ProcessCommands (work from other threads)"]
                               [WAIT=Wait] [THREAD=auto]
Streams tracy-events.csv three times with small memory: finds the thread (default: the one running
CommandProcessor::Process), collects the PARENT intervals, then keeps only zones inside them. For
every WAIT zone nested in PARENT it records the names of the other zones that started earlier in
the same PARENT instance (the "signature") and sums wait time per signature.
"""
import bisect
import collections
import csv
import sys
from pathlib import Path

run = Path(sys.argv[1])
parent_name = sys.argv[2] if len(sys.argv) > 2 else "GuestGpu::ProcessCommands (work from other threads)"
wait_name = sys.argv[3] if len(sys.argv) > 3 else "Wait"
thread = sys.argv[4] if len(sys.argv) > 4 else None
path = run / "tracy-events.csv"


def rows():
    with open(path, newline="") as f:
        yield from csv.DictReader(f)


if thread is None:
    thread = next(r["thread"] for r in rows() if r["name"].endswith("CommandProcessor::Process"))
parents = sorted((int(r["ns_since_start"]), int(r["ns_since_start"]) + int(r["exec_time_ns"]))
                 for r in rows() if r["thread"] == thread and r["name"] == parent_name)
starts = [p[0] for p in parents]
inside = collections.defaultdict(list)
for r in rows():
    if r["thread"] != thread or r["name"] == parent_name:
        continue
    s = int(r["ns_since_start"])
    i = bisect.bisect_right(starts, s) - 1
    if i >= 0 and s <= parents[i][1]:
        inside[i].append((s, s + int(r["exec_time_ns"]), r["name"]))
totals = collections.Counter()
counts = collections.Counter()
for zones in inside.values():
    zones.sort()
    for i, (s, e, n) in enumerate(zones):
        if n != wait_name:
            continue
        sig = tuple(sorted({z[2] for z in zones[:i] if z[2] != wait_name and z[0] < s}))[:6]
        totals[sig] += e - s
        counts[sig] += 1
print(f"thread {thread}: {sum(counts.values())} waits in {len(parents)} '{parent_name}' zones")
for sig, t in totals.most_common(12):
    print(f"{t / 1e9:7.3f} s x{counts[sig]:5d}  preceded by: {', '.join(sig) or '(nothing profiled)'}")
