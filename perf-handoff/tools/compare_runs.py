"""Aggregate run_experiment.py outputs and compare variants, with runs as the independent samples.

Usage: python compare_runs.py OUT.json [--luma LO:HI] [--min-avail-mib N] [--source auto|emu|title]
                              VARIANT=RUN_DIR[,RUN_DIR...] [VARIANT=...]

Validity (every exclusion is reported with its reason):
  run     no run-meta/run-results (incomplete), exited early, unhandled host exception, no valid window
  window  no frame data, window resized during capture, scene check failed (--luma: the screenshots
          taken before and after the window must both have mean luma within LO..HI), available RAM
          below --min-avail-mib
Frame source: "emu" = the emulator's KYTY_FRAME_LOG (exact presentation times, game frames only);
"title" = the window-caption collector (throughput is right, per-frame times are approximate).
"auto" uses emu when the run has it. Mixed sources inside one comparison are flagged.

Per run: FPS = frames / captured seconds over its valid windows; frame-time percentiles pooled over
those windows; per-minute counts normalized by the actual captured duration.
Per variant: mean and spread of run FPS. Differences vs the first variant: mean difference with a
bootstrap over runs (resampling runs, not windows) and, when both variants have the same number
of runs, the paired per-run differences in batch order.
"""
import argparse
import csv
import json
import random
import statistics
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import frame_log  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("variants", nargs="+", metavar="VARIANT=RUN_DIR[,RUN_DIR...]")
ap.add_argument("--luma", help="scene check: LO:HI mean luma of the bracketing screenshots")
ap.add_argument("--min-avail-mib", type=float, default=0)
ap.add_argument("--source", choices=("auto", "emu", "title"), default="auto")
a = ap.parse_args()
luma = tuple(float(x) for x in a.luma.split(":")) if a.luma else None


def pct(v, p):
    s = sorted(v)
    k = (len(s) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def shot_luma(shot):
    if not isinstance(shot, dict):
        return None
    pw = shot.get("printwindow") or {}
    return pw.get("mean_luma")


def load_run(run_dir):
    run_dir = Path(run_dir)
    reasons = []
    if not (run_dir / "run-meta.json").exists() or not (run_dir / "run-results.json").exists():
        return None, ["incomplete: run-meta.json or run-results.json missing"], []
    meta = json.loads((run_dir / "run-meta.json").read_text())
    res = json.loads((run_dir / "run-results.json").read_text())
    if res.get("exited_early"):
        reasons.append(f"emulator exited early (exit code {res.get('exit_code')})")
    if (res.get("log_markers") or {}).get("unhandled"):
        reasons.append("unhandled host exception in the log")
    if reasons:
        return None, reasons, []
    emu_log = run_dir / "frames-emu.csv"
    shots = res.get("screenshots") or []
    wins = res.get("windows") or []
    window_notes = []
    frames, seconds, source_used = [], 0.0, set()
    durations = []
    fps_windows, hard_faults, cpu_cores, busiest, ws, private, avail_min = [], [], [], [], [], [], []
    for i, s in enumerate(wins, 1):
        wdir = run_dir / f"window{i}"
        why = []
        if s.get("client_size_changed_during_window"):
            why.append("window resized during capture")
        if luma:
            for shot in (shots[i - 1] if i - 1 < len(shots) else None, shots[i] if i < len(shots) else None):
                y = shot_luma(shot)
                if y is None or not (luma[0] <= y <= luma[1]):
                    why.append(f"scene check failed (luma {y if y is None else round(y, 1)} outside "
                               f"{luma[0]:g}..{luma[1]:g})")
                    break
        if a.min_avail_mib and s.get("sys_available_mib_min", 1e9) < a.min_avail_mib:
            why.append(f"available RAM fell to {s['sys_available_mib_min']:.0f} MiB")
        use_emu = a.source == "emu" or (a.source == "auto" and emu_log.exists() and "qpc_window" in s)
        iv, dur = None, None
        if use_emu:
            if not emu_log.exists() or "qpc_window" not in s:
                why.append("no emulator frame log for this window")
            else:
                t0, t1 = s["qpc_window"]
                iv = frame_log.intervals_ms(emu_log, t0, t1)
                dur = t1 - t0
                n_frames = len(iv) + 1 if iv else 0
        else:
            fcsv = wdir / "frames.csv"
            if not fcsv.exists() or "fps_frame_counter" not in s:
                why.append("no caption frame data")
            else:
                iv = [float(r["per_frame_ms"]) for r in csv.DictReader(open(fcsv)) if r["per_frame_ms"]]
                dur = s.get("duration_s") or s.get("frame_span_s")
                n_frames = s["fps_frame_counter"] * dur
        if iv is not None and len(iv) < 2:
            why.append("fewer than two frames")
        if why:
            window_notes.append(f"window{i}: " + "; ".join(why))
            continue
        source_used.add("emu" if use_emu else "title")
        frames += iv
        seconds += dur
        durations.append(dur)
        fps_windows.append(n_frames / dur)
        hard_faults.append(s.get("emu_hard_faults_total", 0))
        cpu_cores.append(s.get("emu_cpu_cores_mean"))
        busiest.append(s.get("busiest_thread_pct_of_one_cpu_mean"))
        ws.append(s.get("emu_ws_mib_mean"))
        private.append(s.get("emu_private_mib_mean"))
        avail_min.append(s.get("sys_available_mib_min"))
    if not frames:
        return None, ["no valid window"] + window_notes, window_notes
    slow = sorted(frames, reverse=True)
    n1 = max(1, len(slow) // 100)
    minutes = seconds / 60

    def mean(v):
        v = [x for x in v if x is not None]
        return statistics.fmean(v) if v else None

    return dict(
        run=str(run_dir), label=meta["label"], exe_sha256=meta["exe_sha256"], env=meta.get("env_overrides"),
        hooks=[Path(h).name for h in res.get("hook_modules", [])],
        source=sorted(source_used), valid_windows=len(fps_windows), captured_s=seconds,
        window_fps=fps_windows,
        fps=sum(f * d for f, d in zip(fps_windows, durations)) / seconds,
        frame_ms_p50=pct(frames, 50), frame_ms_p95=pct(frames, 95), frame_ms_p99=pct(frames, 99),
        frame_ms_max=max(frames), low_1pct_fps=1000 / statistics.fmean(slow[:n1]),
        over_50ms_per_min=sum(1 for x in frames if x > 50) / minutes,
        over_100ms_per_min=sum(1 for x in frames if x > 100) / minutes,
        hard_faults_per_min=sum(hard_faults) / minutes,
        emu_cpu_cores=mean(cpu_cores), busiest_thread_pct=mean(busiest),
        emu_ws_mib=mean(ws), emu_private_mib=mean(private),
        sys_available_mib_min=min((x for x in avail_min if x is not None), default=None),
        exit_code=res.get("exit_code"), excluded_windows=window_notes,
        _frames=frames,
    ), [], window_notes


def bootstrap_diff(x, y, n=20000, seed=1):
    rng = random.Random(seed)
    d = []
    for _ in range(n):
        d.append(statistics.fmean(rng.choice(y) for _ in y) - statistics.fmean(rng.choice(x) for _ in x))
    d.sort()
    return d[int(0.025 * n)], d[int(0.975 * n)]


variants, excluded = {}, []
for spec in a.variants:
    name, dirs = spec.split("=", 1)
    runs = []
    for d in dirs.split(","):
        r, why, _ = load_run(d)
        if r is None:
            excluded.append(dict(variant=name, run=d, reasons=why))
        else:
            runs.append(r)
    variants[name] = runs

report = dict(excluded_runs=excluded, variants={}, options=vars(a))
sources = {s for runs in variants.values() for r in runs for s in r["source"]}
if len(sources) > 1:
    report["warning"] = f"frame sources differ between runs: {sorted(sources)}"
base_name = next(iter(variants))
base_fps = [r["fps"] for r in variants[base_name]]
for name, runs in variants.items():
    if not runs:
        report["variants"][name] = dict(n_runs=0)
        continue
    fps = [r["fps"] for r in runs]
    frames = [x for r in runs for x in r["_frames"]]
    slow = sorted(frames, reverse=True)
    v = dict(
        runs=[{k: val for k, val in r.items() if not k.startswith("_")} for r in runs],
        n_runs=len(runs), fps_mean=statistics.fmean(fps),
        fps_run_stdev=statistics.stdev(fps) if len(fps) > 1 else None,
        frame_ms_p50=pct(frames, 50), frame_ms_p95=pct(frames, 95), frame_ms_p99=pct(frames, 99),
        low_1pct_fps=1000 / statistics.fmean(slow[:max(1, len(slow) // 100)]),
        over_100ms_per_min=statistics.fmean(r["over_100ms_per_min"] for r in runs),
    )
    if name != base_name and base_fps:
        ch = dict(mean=v["fps_mean"] - statistics.fmean(base_fps),
                  percent=100 * (v["fps_mean"] / statistics.fmean(base_fps) - 1))
        if len(fps) > 1 and len(base_fps) > 1:
            ch["ci95_bootstrap_over_runs"] = list(bootstrap_diff(base_fps, fps))
        if len(fps) == len(base_fps):
            ch["paired_diffs"] = [y - x for x, y in zip(base_fps, fps)]
        v["fps_change_vs_" + base_name] = ch
    report["variants"][name] = v
Path(a.out).write_text(json.dumps(report, indent=2))

for e in excluded:
    print(f"EXCLUDED {e['variant']}: {Path(e['run']).name}: {'; '.join(e['reasons'])}")
if "warning" in report:
    print("WARNING:", report["warning"])
cols = ["fps_mean", "frame_ms_p50", "frame_ms_p95", "frame_ms_p99", "low_1pct_fps"]
print(f"{'variant':28s} {'runs':>4s} " + " ".join(f"{c:>13s}" for c in cols))
for name, v in report["variants"].items():
    if not v.get("n_runs"):
        print(f"{name:28s}    0  (no valid runs)")
        continue
    print(f"{name:28s} {v['n_runs']:4d} " + " ".join(f"{v[c]:13.2f}" for c in cols))
    ch = next((val for k, val in v.items() if k.startswith("fps_change_vs_")), None)
    if ch:
        extra = ""
        if "ci95_bootstrap_over_runs" in ch:
            lo, hi = ch["ci95_bootstrap_over_runs"]
            extra += f", run bootstrap 95% [{lo:+.2f}, {hi:+.2f}]"
        if "paired_diffs" in ch:
            extra += f", paired {[round(x, 2) for x in ch['paired_diffs']]}"
        print(f"{'':28s} change {ch['mean']:+.2f} FPS ({ch['percent']:+.1f}%){extra}")
    for r in v["runs"]:
        print(f"    {Path(r['run']).name:10s} fps={r['fps']:.2f} src={','.join(r['source'])} "
              f"win={[round(x, 2) for x in r['window_fps']]} captured={r['captured_s']:.0f}s "
              f">100ms/min={r['over_100ms_per_min']:.2f} hot_thr={r['busiest_thread_pct'] or 0:.0f}% "
              f"ws={r['emu_ws_mib'] or 0:.0f} avail_min={r['sys_available_mib_min'] or 0:.0f}"
              + (f" excluded: {r['excluded_windows']}" if r["excluded_windows"] else ""))
