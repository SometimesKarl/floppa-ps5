"""Per-run memory growth and scene check for run_experiment.py outputs.

Usage: growth_report.py [--luma LO:HI] RUN_DIR [RUN_DIR ...]
For each run: private-bytes slope (MiB/s, least squares over window means inside the scene gate),
working set, frame-log FPS per window, and log markers of interest.
"""
import argparse
import json
import re
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("runs", nargs="+")
ap.add_argument("--luma", default="150:200")
a = ap.parse_args()
lo, hi = (float(x) for x in a.luma.split(":"))


def slope(points):
    if len(points) < 2:
        return None
    n = len(points)
    mx = sum(x for x, _ in points) / n
    my = sum(y for _, y in points) / n
    sxx = sum((x - mx) ** 2 for x, _ in points)
    return sum((x - mx) * (y - my) for x, y in points) / sxx if sxx else None


MARKERS = {
    "cpu_arg_reads": r"DrawIndexIndirect: CPU reads the arguments",
    "mesh_clamp": r"exceeded the host instance limit",
    "event_cap": r"Kernel event queue:",
    "file_retry": r"hit protected guest memory",
    "view_recreate": r"recreating a format",
}
for run in a.runs:
    run = Path(run)
    res_path = run / "run-results.json"
    if not res_path.exists():
        print(f"{run.name}: no run-results.json")
        continue
    res = json.loads(res_path.read_text())
    shots = res.get("screenshots") or []
    points, fps = [], []
    for i, w in enumerate(res.get("windows") or [], 1):
        lum = [((shots[k] or {}).get("printwindow") or {}).get("mean_luma") for k in (i - 1, i) if k < len(shots)]
        in_scene = all(x is not None and lo <= x <= hi for x in lum) and len(lum) == 2
        ef = w.get("emu_frames") or {}
        tag = f"{ef['fps']:.1f}" if ef.get("valid") else "-"
        fps.append(tag + ("" if in_scene else "*"))
        if in_scene and "emu_private_mib_mean" in w:
            points.append((w["t_since_launch_s"], w["emu_private_mib_mean"]))
    text = (run / "emulator-stdout.log").read_text(errors="replace") if (run / "emulator-stdout.log").exists() else ""
    marks = {k: len(re.findall(v, text)) for k, v in MARKERS.items()}
    s = slope(points)
    print(f"{run.name}: exit={res.get('exit_code')} early={res.get('exited_early')} "
          f"priv_slope={'%.2f MiB/s' % s if s is not None else 'n/a'} over {len(points)} scene windows "
          f"(priv {points[0][1]:.0f}->{points[-1][1]:.0f})" if points else f"{run.name}: no scene windows",
          f"fps={fps} markers={marks}")
