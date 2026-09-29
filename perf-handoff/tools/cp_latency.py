"""Per-run GPU-thread latency from a frame log with guest_submit_qpc (9th column): time from the
guest's flip submission to the command processor reaching the flip, and to the present.
Usage: cp_latency.py RUN_DIR [RUN_DIR ...]   (uses the run's window QPC ranges when present)"""
import json, statistics, sys
from pathlib import Path
for run in sys.argv[1:]:
    run = Path(run)
    lines = (run / "frames-emu.csv").read_text().splitlines()
    freq = int(lines[0].split("=")[1])
    rows = [list(map(int, l.split(","))) for l in lines[2:] if l and l[0] == "0" and len(l.split(",")) >= 9]
    rows = [r for r in rows if r[8] > 0]
    try:
        res = json.loads((run / "run-results.json").read_text())
        wins = [(w["qpc_window"][0] * freq, w["qpc_window"][1] * freq) for w in res["windows"] if w.get("qpc_window")]
        busy = [w.get("busiest_thread_pct_of_one_cpu_mean", 0) for w in res["windows"]]
        cores = [w.get("emu_cpu_cores_mean", 0) for w in res["windows"]]
    except Exception:
        wins, busy, cores = [], [], []
    if wins:
        rows = [r for r in rows if any(a <= r[7] <= b for a, b in wins)]
    cp = sorted((r[4] - r[8]) * 1000 / freq for r in rows)
    tot = sorted((r[7] - r[8]) * 1000 / freq for r in rows)
    iv = sorted((rows[i][7] - rows[i - 1][7]) * 1000 / freq for i in range(1, len(rows)))
    print(f"{run.name:4s} frames {len(rows):5d}  submit->CP flip p50 {statistics.median(cp):5.1f} p90 {cp[int(len(cp)*.9)]:5.1f}"
          f"  submit->screen p50 {statistics.median(tot):5.1f}  frame p50 {statistics.median(iv):5.1f} p90 {iv[int(len(iv)*.9)]:5.1f}"
          f"  busiest thread {statistics.mean(busy) if busy else 0:5.1f}%  cores {statistics.mean(cores) if cores else 0:4.2f}")
