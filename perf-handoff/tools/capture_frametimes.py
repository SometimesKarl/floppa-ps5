"""Read-only frame-time and memory-pressure collector for kyty_emulator.

Frame times: the emulator rewrites its window caption after every successful
vkQueuePresentKHR (swapchain.cpp Presenter::Present -> WindowContext::UpdateTitle),
embedding a monotonically increasing "frame: N". GetWindowTextW on a window owned
by another process reads the cached caption without sending it a message, so
polling it yields a host-present timestamp per frame without injecting anything.
Resolution is bounded by the measured poll interval (reported in the summary).

Memory/CPU: NtQuerySystemInformation(SystemProcessInformation) once per second,
unprivileged. Gives per-process HardFaultCount (includes compression-store and
pagefile reads; does NOT separate them), working set, commit, per-thread CPU and
context switches, and the Memory Compression process working set.

Usage: python capture_frametimes.py PID SECONDS OUTDIR [--label TEXT]
Nothing is written outside OUTDIR. The target process is never signalled.
"""
import ctypes as c
import ctypes.wintypes as w
import csv
import datetime
import json
import os
import re
import statistics
import struct
import sys
import threading
import time
from pathlib import Path

if len(sys.argv) < 4:
    raise SystemExit("Usage: python capture_frametimes.py PID SECONDS OUTDIR [--label TEXT]")
PID = int(sys.argv[1])
SECONDS = float(sys.argv[2])
OUT = Path(sys.argv[3])
LABEL = sys.argv[sys.argv.index("--label") + 1] if "--label" in sys.argv else ""
OUT.mkdir(parents=True, exist_ok=True)

user32 = c.WinDLL("user32", use_last_error=True)
kernel32 = c.WinDLL("kernel32", use_last_error=True)
ntdll = c.WinDLL("ntdll")
winmm = c.WinDLL("winmm")

EnumWindowsProc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, w.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user32.IsWindowVisible.argtypes = [w.HWND]
ntdll.NtQuerySystemInformation.argtypes = [w.ULONG, c.c_void_p, w.ULONG, c.POINTER(w.ULONG)]
ntdll.NtQuerySystemInformation.restype = c.c_long
kernel32.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenThread.restype = w.HANDLE
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.GetThreadDescription.argtypes = [w.HANDLE, c.POINTER(c.c_void_p)]
kernel32.LocalFree.argtypes = [c.c_void_p]

FRAME_RE = re.compile(r"frame: (\d+), fps: (\d+)")


def find_window(pid):
    found = []

    def cb(hwnd, _):
        owner = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buf = c.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buf, 512)
            if FRAME_RE.search(buf.value):
                found.append(hwnd)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found[0] if found else None


# ---- NtQuerySystemInformation helpers (x64 layouts) ----
SPI_SIZE, STI_SIZE = 0x100, 0x50


def query_processes():
    size = 1 << 20
    while True:
        buf = c.create_string_buffer(size)
        ret = w.ULONG()
        status = ntdll.NtQuerySystemInformation(5, buf, size, c.byref(ret)) & 0xFFFFFFFF
        if status == 0xC0000004:
            size = max(size * 2, ret.value + 65536)
            continue
        if status != 0:
            raise OSError(f"NtQuerySystemInformation(5) status {status:#x}")
        break
    raw = buf.raw
    base = c.addressof(buf)
    procs = {}
    off = 0
    while True:
        nxt, nthreads = struct.unpack_from("<II", raw, off)
        hard_faults, = struct.unpack_from("<I", raw, off + 16)
        cycle, = struct.unpack_from("<Q", raw, off + 24)
        user_t, kern_t = struct.unpack_from("<qq", raw, off + 40)
        name_len, = struct.unpack_from("<H", raw, off + 56)
        name_ptr, = struct.unpack_from("<Q", raw, off + 64)
        pid, = struct.unpack_from("<Q", raw, off + 80)
        page_faults, = struct.unpack_from("<I", raw, off + 128)
        ws, = struct.unpack_from("<Q", raw, off + 144)
        private, = struct.unpack_from("<Q", raw, off + 200)
        name = ""
        if name_ptr and name_len:
            name = raw[name_ptr - base: name_ptr - base + name_len].decode("utf-16-le", "replace")
        threads = {}
        for i in range(nthreads):
            t = off + SPI_SIZE + i * STI_SIZE
            tk, tu = struct.unpack_from("<qq", raw, t)
            start_addr, = struct.unpack_from("<Q", raw, t + 32)
            tid, = struct.unpack_from("<Q", raw, t + 48)
            cswitch, = struct.unpack_from("<I", raw, t + 64)
            threads[tid] = (tk + tu, cswitch, start_addr)
        procs[pid] = dict(name=name, hard_faults=hard_faults, page_faults=page_faults, ws=ws,
                          private=private, cpu=user_t + kern_t, cycle=cycle, threads=threads)
        if nxt == 0:
            break
        off += nxt
    return procs


def query_perf():
    buf = c.create_string_buffer(0x1000)
    ret = w.ULONG()
    status = ntdll.NtQuerySystemInformation(2, buf, 0x1000, c.byref(ret)) & 0xFFFFFFFF
    if status != 0:
        return None
    r = buf.raw
    f = lambda o: struct.unpack_from("<I", r, o)[0]
    return dict(available_pages=f(44), page_faults=f(60), transition=f(68), demand_zero=f(76),
                page_read=f(80), page_read_io=f(84), dirty_write=f(96), dirty_write_io=f(100))


def thread_description(tid):
    h = kernel32.OpenThread(0x0040, False, tid)  # GetThreadDescription needs THREAD_QUERY_INFORMATION
    if not h:
        return ""
    try:
        p = c.c_void_p()
        if kernel32.GetThreadDescription(h, c.byref(p)) >= 0 and p.value:
            s = c.wstring_at(p.value)
            kernel32.LocalFree(p)
            return s
    finally:
        kernel32.CloseHandle(h)
    return ""


# ---- sampling ----
hwnd = find_window(PID)
if not hwnd:
    raise SystemExit(f"No visible window with a frame counter found for PID {PID}")

stop = threading.Event()
mem_rows = []
first_procs = query_processes()
if PID not in first_procs:
    raise SystemExit(f"PID {PID} not running")
memc_pid = next((p for p, v in first_procs.items() if v["name"] in ("MemCompression", "Memory Compression")), None)
thread_names = {tid: thread_description(tid) for tid in first_procs[PID]["threads"]}


def sampler():
    prev_t = time.perf_counter()
    prev = query_processes()
    prev_perf = query_perf()
    t0 = prev_t
    while not stop.wait(1.0):
        now = time.perf_counter()
        procs = query_processes()
        perf = query_perf()
        dt = now - prev_t
        cur, old = procs.get(PID), prev.get(PID)
        if cur is None:
            break
        row = dict(
            t_s=round(now - t0, 3),
            emu_ws_mib=cur["ws"] / 2**20, emu_private_mib=cur["private"] / 2**20,
            emu_hard_faults_per_s=(cur["hard_faults"] - old["hard_faults"]) / dt,
            emu_page_faults_per_s=(cur["page_faults"] - old["page_faults"]) / dt,
            emu_cpu_cores=(cur["cpu"] - old["cpu"]) / 1e7 / dt,
            emu_threads=len(cur["threads"]),
            memcompression_ws_mib=procs[memc_pid]["ws"] / 2**20 if memc_pid in procs else None,
        )
        # busiest thread this second (share of one logical CPU)
        busiest = max(((tid, v[0] - old["threads"].get(tid, (v[0],))[0]) for tid, v in cur["threads"].items()),
                      key=lambda x: x[1], default=(0, 0))
        row["busiest_thread_id"] = busiest[0]
        row["busiest_thread_pct_of_one_cpu"] = 100 * busiest[1] / 1e7 / dt
        if perf and prev_perf:
            row.update(
                sys_available_mib=perf["available_pages"] * 4096 / 2**20,
                sys_page_read_pages_per_s=(perf["page_read"] - prev_perf["page_read"]) / dt,
                sys_page_read_ios_per_s=(perf["page_read_io"] - prev_perf["page_read_io"]) / dt,
                sys_transition_faults_per_s=(perf["transition"] - prev_perf["transition"]) / dt,
                sys_demand_zero_per_s=(perf["demand_zero"] - prev_perf["demand_zero"]) / dt,
                sys_dirty_write_pages_per_s=(perf["dirty_write"] - prev_perf["dirty_write"]) / dt,
            )
        mem_rows.append(row)
        prev, prev_perf, prev_t = procs, perf, now


winmm.timeBeginPeriod(1)
sampler_thread = threading.Thread(target=sampler, daemon=True)
start_wall = datetime.datetime.now().astimezone()
start_procs = query_processes()
sampler_thread.start()

frames = []  # (t_s, frame_number, fps_title); only caption *changes*, so every entry is a boundary
polls = 0
max_poll_gap = 0.0
buf = c.create_unicode_buffer(512)
own_cpu0 = time.process_time()
t0 = time.perf_counter()
last_frame = None
last_poll = t0
end = t0 + SECONDS
while True:
    now = time.perf_counter()
    if now >= end:
        break
    max_poll_gap = max(max_poll_gap, now - last_poll)
    last_poll = now
    user32.GetWindowTextW(hwnd, buf, 512)
    polls += 1
    m = FRAME_RE.search(buf.value)
    if m:
        fn = int(m.group(1))
        if last_frame is not None and fn != last_frame:
            frames.append((now - t0, fn, int(m.group(2))))
        last_frame = fn
    time.sleep(0.0005)
elapsed = time.perf_counter() - t0
own_cpu = time.process_time() - own_cpu0
stop.set()
sampler_thread.join(timeout=3)
winmm.timeEndPeriod(1)
end_procs = query_processes()
end_wall = datetime.datetime.now().astimezone()

# ---- frame statistics ----
intervals = []  # per-frame ms, splitting multi-frame gaps evenly
multi = 0
with (OUT / "frames.csv").open("w", newline="") as f:
    wr = csv.writer(f)
    wr.writerow(["t_s", "frame", "delta_frames", "interval_ms", "per_frame_ms", "title_fps"])
    for i, (t, fn, fps) in enumerate(frames):
        if i == 0:
            wr.writerow([f"{t:.6f}", fn, "", "", "", fps])
            continue
        dt_ms = (t - frames[i - 1][0]) * 1000
        df = fn - frames[i - 1][1]
        if df <= 0:
            continue
        if df > 1:
            multi += 1
        per = dt_ms / df
        intervals.extend([per] * df)
        wr.writerow([f"{t:.6f}", fn, df, f"{dt_ms:.3f}", f"{per:.3f}", fps])

if mem_rows:
    with (OUT / "memory-1s.csv").open("w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(mem_rows[-1].keys()))
        wr.writeheader()
        for r in mem_rows:
            wr.writerow(r)


def pct(v, p):
    if not v:
        return None
    s = sorted(v)
    k = (len(s) - 1) * p / 100
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


summary = dict(label=LABEL, pid=PID, start=start_wall.isoformat(), end=end_wall.isoformat(),
               duration_s=elapsed, polls=polls, mean_poll_interval_ms=1000 * elapsed / max(polls, 1),
               max_poll_gap_ms=1000 * max_poll_gap,
               collector_cpu_pct_of_one_cpu=100 * own_cpu / elapsed)
if len(frames) >= 2:
    span = frames[-1][0] - frames[0][0]
    nfr = frames[-1][1] - frames[0][1]
    slowest = sorted(intervals, reverse=True)
    n1 = max(1, len(slowest) // 100)
    summary.update(
        frames=nfr, frame_span_s=span, fps_frame_counter=nfr / span if span > 0 else None,
        multi_frame_gaps=multi,
        frame_ms_mean=statistics.fmean(intervals), frame_ms_median=pct(intervals, 50),
        frame_ms_p90=pct(intervals, 90), frame_ms_p95=pct(intervals, 95), frame_ms_p99=pct(intervals, 99),
        frame_ms_max=max(intervals), frame_ms_min=min(intervals),
        frame_ms_stdev=statistics.pstdev(intervals),
        low_1pct_fps_avg_of_slowest_1pct=1000 / statistics.fmean(slowest[:n1]),
        frames_over_100ms=sum(1 for x in intervals if x > 100),
        frames_over_200ms=sum(1 for x in intervals if x > 200),
    )
if mem_rows:
    for key in [k for k in mem_rows[-1] if k not in ("t_s", "busiest_thread_id")]:
        vals = [r[key] for r in mem_rows if r.get(key) is not None]
        if vals:
            summary[f"{key}_mean"] = statistics.fmean(vals)
            summary[f"{key}_max"] = max(vals)
            summary[f"{key}_min"] = min(vals)
a, b = start_procs.get(PID), end_procs.get(PID)
if a and b:
    summary["emu_hard_faults_total"] = b["hard_faults"] - a["hard_faults"]
    rows = []
    for tid, (cpu1, cs1, start_addr) in b["threads"].items():
        cpu0, cs0, _ = a["threads"].get(tid, (None, None, None))
        if cpu0 is None:
            continue
        rows.append(dict(thread_id=tid, name=thread_names.get(tid, "") or thread_description(tid),
                         start_address=f"{start_addr:#x}", cpu_s=(cpu1 - cpu0) / 1e7,
                         pct_of_one_cpu=100 * (cpu1 - cpu0) / 1e7 / elapsed,
                         context_switches_per_s=(cs1 - cs0) / elapsed))
    rows.sort(key=lambda r: r["cpu_s"], reverse=True)
    with (OUT / "threads.csv").open("w", newline="") as f:
        wr = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        wr.writeheader()
        wr.writerows(rows)
    summary["top_threads"] = rows[:8]
(OUT / "frametime-summary.json").write_text(json.dumps(summary, indent=2))
print(json.dumps({k: v for k, v in summary.items() if k != "top_threads"}, indent=2))
