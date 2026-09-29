"""Summarize an emulator frame log (KYTY_FRAME_LOG, one CSV line per host presentation).

Usage: frame_log.py FRAMES_CSV [T0_QPC_S T1_QPC_S]
T0/T1 are QueryPerformanceCounter seconds (run_experiment.py records them per window as
"qpc_window"); without them the whole log is summarized.

Columns: kind (0 GPU flip, 1 CPU flip, 2 repeated last frame, 3 blank), id, flip_arg, index,
submit_qpc (guest flip request), ready_qpc (GPU finished the flip's command buffer),
present_begin_qpc, present_end_qpc (vkQueuePresent returned).

Frame intervals are between consecutive game-frame presentations (kinds 0/1), so they are exact
per-frame times rather than the title-caption estimate. Repeats are counted separately.
"""
import json
import statistics
import sys
from pathlib import Path

GAME_KINDS = (0, 1)


def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    k = (len(s) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def load(path):
    path = Path(path)
    freq = None
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("# qpc_frequency="):
                freq = int(line.split("=", 1)[1])
                continue
            if not line or line[0] not in "0123456789":
                continue
            parts = line.rstrip("\n").split(",")
            if len(parts) < 8:
                continue  # a line cut off by a crash or forced exit
            rows.append(tuple(int(x) for x in parts[:8]))
    if freq is None:
        raise ValueError(f"{path}: no qpc_frequency header")
    return freq, rows


def stats_ms(v):
    if not v:
        return None
    return dict(n=len(v), mean=statistics.fmean(v), p50=pct(v, 50), p90=pct(v, 90), p99=pct(v, 99), max=max(v))


def select(path, t0=None, t1=None):
    freq, rows = load(path)
    to_s = 1.0 / freq
    return to_s, [r for r in rows if (t0 is None or r[7] * to_s >= t0) and (t1 is None or r[7] * to_s <= t1)]


def intervals_ms(path, t0=None, t1=None):
    """Exact game-frame intervals (ms) presented within [t0, t1]."""
    to_s, sel = select(path, t0, t1)
    ends = [r[7] * to_s for r in sel if r[0] in GAME_KINDS]
    return [(b - a) * 1000 for a, b in zip(ends, ends[1:])]


def summarize(path, t0=None, t1=None):
    to_s, sel = select(path, t0, t1)
    game = [r for r in sel if r[0] in GAME_KINDS]
    repeats = sum(1 for r in sel if r[0] == 2)
    blanks = sum(1 for r in sel if r[0] == 3)
    out = dict(frames=len(game), repeats=repeats, blanks=blanks)
    if len(game) < 2:
        out["valid"] = False
        out["reason"] = "fewer than two game frames in range"
        return out
    ends = [r[7] * to_s for r in game]
    iv = [(b - a) * 1000 for a, b in zip(ends, ends[1:])]
    span = (t1 - t0) if (t0 is not None and t1 is not None) else (ends[-1] - ends[0])
    worst = sorted(iv, reverse=True)
    n1 = max(1, len(worst) // 100)
    # 60 Hz vblank multiples: a frame shown for 1, 2, 3 or more refresh intervals.
    buckets = dict(le_1vb=0, le_2vb=0, le_3vb=0, gt_3vb=0)
    for x in iv:
        if x <= 16.67 * 1.25:
            buckets["le_1vb"] += 1
        elif x <= 16.67 * 2.25:
            buckets["le_2vb"] += 1
        elif x <= 16.67 * 3.25:
            buckets["le_3vb"] += 1
        else:
            buckets["gt_3vb"] += 1
    gpu_eop = [r for r in game if r[0] == 0 and r[5] and r[4]]
    submit = [r[4] * to_s for r in game if r[4]]
    out.update(
        valid=True,
        span_s=span,
        fps=len(game) / span if span > 0 else None,
        frame_ms=stats_ms(iv),
        low_1pct_fps=1000 / statistics.fmean(worst[:n1]),
        over_50ms_per_min=sum(1 for x in iv if x > 50) * 60 / span if span > 0 else None,
        over_100ms_per_min=sum(1 for x in iv if x > 100) * 60 / span if span > 0 else None,
        vblank_buckets=buckets,
        # Guest cadence: time between consecutive flip requests (CPU side of the game loop).
        submit_interval_ms=stats_ms([(b - a) * 1000 for a, b in zip(submit, submit[1:])]),
        # GPU flips: request -> GPU done (host GPU latency for the frame's last command buffer),
        # GPU done -> presentation start (queueing behind vblank/previous present), present call.
        submit_to_ready_ms=stats_ms([(r[5] - r[4]) * to_s * 1000 for r in gpu_eop]),
        ready_to_present_ms=stats_ms([(r[6] - r[5]) * to_s * 1000 for r in gpu_eop]),
        present_call_ms=stats_ms([(r[7] - r[6]) * to_s * 1000 for r in game]),
        flip_arg_repeats=sum(1 for a, b in zip(game, game[1:]) if a[2] == b[2] and a[2] != 0),
    )
    return out


if __name__ == "__main__":
    t0 = float(sys.argv[2]) if len(sys.argv) > 3 else None
    t1 = float(sys.argv[3]) if len(sys.argv) > 3 else None
    print(json.dumps(summarize(sys.argv[1], t0, t1), indent=2))
