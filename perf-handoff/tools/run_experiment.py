"""Unattended, reproducible emulator run with frame-time/GPU/memory capture.

Launches kyty_emulator with the launcher's exact recorded argument list
(Performance Handoff/system-and-launch.json), in an isolated working directory
so the user's Fixed Build runtime data and saves are never touched. No input is
sent to the game. After --capture-at seconds it records N windows with
capture_frametimes.py and the handoff's Capture-Usage.ps1 (GPU), takes window
screenshots between windows, snapshots loaded modules, then closes the window
with WM_CLOSE (TerminateProcess only if it does not exit).

Example:
  python run_experiment.py --label A1-baseline --exe "...\\Fixed Build\\kyty_emulator.exe" \
      --cwd "...\\testbed\\baseline" --out "...\\E01\\A1" --capture-at 240 --windows 3 --window-s 60 \
      [--env DISABLE_RTSS_LAYER=1 ...] [--set-arg --vulkan-validation false] [--add-arg --profile]
"""
import argparse
import ctypes as c
import ctypes.wintypes as w
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
TRACY = Path(r"C:\Users\himav\Desktop\kyty ps5\Performance Experiments\tools\tracy-0.14.1")
HANDOFF_LAUNCH = Path(r"C:\Users\himav\Desktop\kyty ps5\Performance Handoff\system-and-launch.json")

ap = argparse.ArgumentParser()
ap.add_argument("--label", required=True)
ap.add_argument("--exe", required=True)
ap.add_argument("--cwd", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--capture-at", type=float, default=240)
ap.add_argument("--windows", type=int, default=3)
ap.add_argument("--window-s", type=float, default=60)
ap.add_argument("--env", action="append", default=[])
ap.add_argument("--set-arg", nargs=2, action="append", default=[], metavar=("NAME", "VALUE"))
ap.add_argument("--add-arg", action="append", default=[])
ap.add_argument("--no-gpu", action="store_true", help="skip the PowerShell GPU collector")
ap.add_argument("--keep-running", action="store_true", help="do not close the emulator at the end")
ap.add_argument("--allow-input", action="store_true",
                help="leave the emulator window enabled for user keyboard/mouse input")
ap.add_argument("--key-at", action="append", default=[], metavar="SECONDS:KEY[:HOLD_S]",
                help="post a key press to the emulator window at that time since launch, e.g. 60:J or "
                     "260:W:3 (held 3 s); default bindings: J=Cross, WASD=left stick, RETURN=Options; "
                     "repeatable; sent from a background thread, so also during capture windows")
ap.add_argument("--wait-luma", metavar="LO:HI",
                help="scene gate: from --capture-at, poll the window's mean luma every --advance-every s "
                     "(pressing --advance-key while outside LO..HI) until two checks in a row are inside, "
                     "then wait --settle s before the capture windows")
ap.add_argument("--gate-stable", type=float, default=2.0,
                help="scene gate: consecutive in-range checks must also agree within this luma "
                     "(a static scene; moving cutscenes of similar brightness do not pass)")
ap.add_argument("--advance-key", default="J", help="key pressed while the scene gate is not reached")
ap.add_argument("--advance-every", type=float, default=4.0)
ap.add_argument("--settle", type=float, default=20.0)
ap.add_argument("--wait-timeout", type=float, default=600.0)
ap.add_argument("--no-frame-log", action="store_true",
                help="do not set KYTY_FRAME_LOG (the emulator-side per-presentation log)")
ap.add_argument("--tracy-events", action="store_true",
                help="also export every Tracy zone event (tracy-events.csv, several GB per 20 s trace)")
ap.add_argument("--tracy-seconds", type=float, default=0,
                help="with --add-arg --profile: record a Tracy trace during window 1 (attribution only)")
a = ap.parse_args()

out = Path(a.out)
out.mkdir(parents=True, exist_ok=True)
cwd = Path(a.cwd)
cwd.mkdir(parents=True, exist_ok=True)

user32 = c.WinDLL("user32", use_last_error=True)
user32.SetProcessDPIAware()
kernel32 = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
EnumWindowsProc = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user32.EnumWindows.argtypes = [EnumWindowsProc, w.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user32.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenProcess.restype = w.HANDLE
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]


kernel32.QueryPerformanceCounter.argtypes = [c.POINTER(c.c_longlong)]
kernel32.QueryPerformanceFrequency.argtypes = [c.POINTER(c.c_longlong)]


def qpc_s():
    """QueryPerformanceCounter in seconds: the clock of the emulator's KYTY_FRAME_LOG."""
    v, f = c.c_longlong(), c.c_longlong()
    kernel32.QueryPerformanceCounter(c.byref(v))
    kernel32.QueryPerformanceFrequency(c.byref(f))
    return v.value / f.value


class MEMSTAT(c.Structure):
    _fields_ = [("dwLength", w.DWORD), ("dwMemoryLoad", w.DWORD)] + [
        (n, c.c_ulonglong) for n in ("total", "avail", "totalPage", "availPage", "totalVirt", "availVirt", "availExt")]


def memstat():
    m = MEMSTAT()
    m.dwLength = c.sizeof(m)
    kernel32.GlobalMemoryStatusEx(c.byref(m))
    return dict(available_mib=m.avail / 2**20, total_mib=m.total / 2**20,
                commit_mib=(m.totalPage - m.availPage) / 2**20, commit_limit_mib=m.totalPage / 2**20)


def log(msg):
    line = f"[{datetime.datetime.now().astimezone().isoformat(timespec='seconds')}] {msg}"
    print(line, flush=True)
    with (out / "run.log").open("a", encoding="utf-8") as f:
        f.write(line + "\n")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_window(pid):
    found = []

    def cb(hwnd, _):
        owner = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            buf = c.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, buf, 512)
            found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    for hwnd, title in found:
        if "frame:" in title:
            return hwnd, title
    return (found[0] if found else (None, ""))


user32.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user32.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
user32.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
user32.IsIconic.argtypes = [w.HWND]
user32.ShowWindow.argtypes = [w.HWND, c.c_int]


def client_size(hwnd):
    rc = w.RECT()
    user32.GetClientRect(hwnd, c.byref(rc))
    return rc.right - rc.left, rc.bottom - rc.top


def restore_client_size(hwnd, width, height):
    """Undo resizes/minimize of the test window so every window is measured at the launch size."""
    if user32.IsIconic(hwnd):
        user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    cw, ch = client_size(hwnd)
    if (cw, ch) == (width, height):
        return False
    wr = w.RECT()
    user32.GetWindowRect(hwnd, c.byref(wr))
    outer_w = (wr.right - wr.left) - cw + width
    outer_h = (wr.bottom - wr.top) - ch + height
    user32.SetWindowPos(hwnd, None, 0, 0, outer_w, outer_h, 0x0002 | 0x0004 | 0x0010)  # NOMOVE|NOZORDER|NOACTIVATE
    return True


def modules(pid):
    h = kernel32.OpenProcess(0x0410, False, pid)
    if not h:
        return []
    try:
        arr = (w.HMODULE * 2048)()
        needed = w.DWORD()
        if not psapi.EnumProcessModulesEx(h, arr, c.sizeof(arr), c.byref(needed), 3):
            return []
        names = []
        for i in range(needed.value // c.sizeof(w.HMODULE)):
            buf = c.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(h, arr[i], buf, 1024)
            names.append(buf.value)
        return names
    finally:
        kernel32.CloseHandle(h)


def top_processes(n=15):
    ps = subprocess.run(["powershell", "-NoProfile", "-Command",
                         "Get-Process | Sort-Object WorkingSet64 -Descending | Select-Object -First %d "
                         "Id,ProcessName,@{n='WS_MiB';e={[int]($_.WorkingSet64/1MB)}},"
                         "@{n='Priv_MiB';e={[int]($_.PrivateMemorySize64/1MB)}} | ConvertTo-Json" % n],
                        capture_output=True, text=True)
    try:
        return json.loads(ps.stdout)
    except Exception:
        return ps.stdout


# ---- launch arguments: exact launcher list, with explicit, recorded overrides ----
launch = json.loads(HANDOFF_LAUNCH.read_text(encoding="utf-8-sig"))
tokens = re.findall(r'"([^"]*)"', launch["Process"]["CommandLine"])
args = tokens[1:]
for name, value in a.set_arg:
    i = args.index(name)
    args[i + 1] = value
args += a.add_arg
env = os.environ.copy()
if not a.allow_input:
    # Test instances ignore game controllers: SDL reads them directly, so a controller the user
    # touches (or one with stick drift) would otherwise move a run off the measured scene.
    for hint in ("SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI",
                 "SDL_JOYSTICK_DIRECTINPUT", "SDL_JOYSTICK_GAMEINPUT", "SDL_XINPUT_ENABLED"):
        env.setdefault(hint, "0")
if not a.no_frame_log:
    env.setdefault("KYTY_FRAME_LOG", str(out / "frames-emu.csv"))
for kv in a.env:
    k, v = kv.split("=", 1)
    env[k] = v

exe = Path(a.exe)
meta = dict(label=a.label, exe=str(exe), exe_sha256=sha256(exe), exe_size=exe.stat().st_size,
            cwd=str(cwd), args=args, env_overrides=a.env, set_args=a.set_arg, add_args=a.add_arg,
            capture_at_s=a.capture_at, windows=a.windows, window_s=a.window_s, key_at=a.key_at,
            patch_sha256=sha256(args[args.index("--game-patch") + 1]),
            game_eboot_sha256=sha256(args[args.index("--game") + 1]),
            pre_memory=memstat(), pre_top_processes=top_processes())
save_dir = cwd / "_SaveData"
if save_dir.exists():
    meta["pre_saves"] = {str(p.relative_to(cwd)): sha256(p) for p in save_dir.rglob("*") if p.is_file()}
(out / "run-meta.json").write_text(json.dumps(meta, indent=2))
log(f"launch {exe.name} sha256={meta['exe_sha256'][:16]}.. cwd={cwd}")

stdout_f = open(out / "emulator-stdout.log", "wb")
stderr_f = open(out / "emulator-stderr.log", "wb")
t_launch = time.perf_counter()
proc = subprocess.Popen([str(exe)] + args, cwd=str(cwd), env=env, stdout=stdout_f, stderr=stderr_f,
                        creationflags=0x08000000)  # CREATE_NO_WINDOW (console app; output goes to files)
pid = proc.pid
log(f"pid {pid}")

timeline = open(out / "boot-timeline.csv", "w", buffering=1)
timeline.write("t_since_launch_s,frame,title_fps,available_mib\n")
first_frame_t = None
results = dict(pid=pid)


user32.EnableWindow.argtypes = [w.HWND, w.BOOL]
input_locked = set()


def lock_input(hwnd):
    # A test window that grabs focus would otherwise receive whatever the user types elsewhere,
    # which moved runs off the measured scene. A disabled window still renders and still
    # processes messages posted by --key-at.
    if a.allow_input or not hwnd or hwnd in input_locked:
        return
    user32.EnableWindow(hwnd, False)
    input_locked.add(hwnd)
    log("user input to the emulator window disabled")


def poll_title():
    global first_frame_t
    hwnd, title = find_window(pid)
    m = re.search(r"frame: (\d+), fps: (\d+)", title or "")
    t = time.perf_counter() - t_launch
    if m:
        lock_input(hwnd)
        if first_frame_t is None:
            first_frame_t = t
            log(f"first frame counter at {t:.1f}s")
        timeline.write(f"{t:.1f},{m.group(1)},{m.group(2)},{memstat()['available_mib']:.0f}\n")
    return hwnd, m


VK_NAMES = {"RETURN": 0x0D, "SPACE": 0x20, "ESCAPE": 0x1B, "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27,
            "DOWN": 0x28}
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.MapVirtualKeyW.argtypes = [w.UINT, w.UINT]
user32.MapVirtualKeyW.restype = w.UINT


def parse_key(spec):
    parts = spec.split(":")
    seconds, key = float(parts[0]), parts[1].upper()
    hold = float(parts[2]) if len(parts) > 2 else 0.15
    vk = VK_NAMES.get(key, ord(key) if len(key) == 1 else None)
    if vk is None:
        raise SystemExit(f"unknown key in --key-at {spec}")
    return [(seconds, vk, True), (seconds + hold, vk, False)]


key_events = sorted(e for spec in a.key_at for e in parse_key(spec))


def post_key(vk, down):
    # SDL reads WM_KEYDOWN/WM_KEYUP with the scan code in lParam; the window need not be focused.
    hwnd, _ = find_window(pid)
    if not hwnd:
        log(f"key vk=0x{vk:02x}: no window")
        return
    scan = user32.MapVirtualKeyW(vk, 0)
    if down:
        user32.PostMessageW(hwnd, 0x0100, vk, 1 | (scan << 16))
    else:
        user32.PostMessageW(hwnd, 0x0101, vk, 1 | (scan << 16) | (1 << 30) | (1 << 31))
    t = time.perf_counter() - t_launch
    key_log.append((round(t, 3), qpc_s(), vk, down))
    log(f"key vk=0x{vk:02x} {'down' if down else 'up'} at {t:.1f}s")


key_log = []


def key_thread():
    for t_event, vk, down in key_events:
        while time.perf_counter() - t_launch < t_event:
            if proc.poll() is not None:
                return
            time.sleep(0.01)
        post_key(vk, down)


if key_events:
    threading.Thread(target=key_thread, daemon=True).start()


def wait_until(t_target):
    while time.perf_counter() - t_launch < t_target:
        if proc.poll() is not None:
            raise RuntimeError(f"emulator exited early with code {proc.returncode}")
        poll_title()
        time.sleep(1.0)


def screenshot(name):
    r = subprocess.run([sys.executable, str(TOOLS / "capture_window.py"), str(pid), str(out / name)],
                       capture_output=True, text=True)
    log(f"screenshot {name}: {r.stdout.strip() or r.stderr.strip()}")
    try:
        return json.loads(r.stdout)
    except Exception:
        return None


def shot_luma(name):
    shot = screenshot(name)
    return ((shot or {}).get("printwindow") or {}).get("mean_luma")


def wait_for_scene():
    lo, hi = (float(x) for x in a.wait_luma.split(":"))
    vk = VK_NAMES.get(a.advance_key.upper(), ord(a.advance_key.upper()))
    t_start = time.perf_counter() - t_launch
    inside = 0
    n = 0
    previous = None
    while time.perf_counter() - t_launch < t_start + a.wait_timeout:
        if proc.poll() is not None:
            raise RuntimeError(f"emulator exited early with code {proc.returncode}")
        y = shot_luma(f"gate-{n:03d}.png")
        n += 1
        if y is not None and lo <= y <= hi:
            stable = previous is not None and abs(y - previous) <= a.gate_stable
            previous = y
            inside = inside + 1 if stable or inside == 0 else 1
            if inside >= 2:
                t = time.perf_counter() - t_launch
                log(f"scene gate reached at {t:.1f}s (luma {y:.1f}); settling {a.settle:.0f}s")
                results["scene_gate_s"] = t
                wait_until(t + a.settle)
                return
        else:
            inside = 0
            previous = y
            post_key(vk, True)
            time.sleep(0.15)
            post_key(vk, False)
        wait_until(time.perf_counter() - t_launch + a.advance_every)
    raise RuntimeError(f"scene gate {a.wait_luma} not reached within {a.wait_timeout:.0f}s")


try:
    wait_until(a.capture_at)
    if a.wait_luma:
        wait_for_scene()
    mods = modules(pid)
    (out / "loaded-modules.txt").write_text("\n".join(mods))
    hooks = [m for m in mods if re.search(r"medal|rtss|graphics-hook|VkLayer|obs|overlay|steam|EOSOVH|discord|hook",
                                          Path(m).name, re.I)]
    results["hook_modules"] = hooks
    results["validation_layer_loaded"] = any(re.search(r"VkLayer_khronos_validation", m, re.I) for m in mods)
    log(f"hook modules: {hooks}")
    results["screenshots"] = [screenshot("shot-0-before.png")]
    windows = results["windows"] = []  # kept even if a later window fails
    want_w = int(args[args.index("--screen-width") + 1])
    want_h = int(args[args.index("--screen-height") + 1])
    for i in range(1, a.windows + 1):
        wdir = out / f"window{i}"
        wdir.mkdir(exist_ok=True)
        hwnd, _ = find_window(pid)
        if hwnd and restore_client_size(hwnd, want_w, want_h):
            log(f"window {i}: test window had been resized/minimized; restored to {want_w}x{want_h}")
            time.sleep(5)
        size_before = client_size(hwnd) if hwnd else None
        shutil.copy2(TOOLS / "capture_frametimes.py", wdir)
        gpu = None
        if not a.no_gpu:
            shutil.copy2(TOOLS / "Capture-Usage.ps1", wdir)
            gpu = subprocess.Popen(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                    str(wdir / "Capture-Usage.ps1"), "-TargetProcessId", str(pid),
                                    "-Samples", str(int(a.window_s // 2)), "-IntervalSeconds", "2"],
                                   stdout=open(wdir / "gpu-collector.log", "w"), stderr=subprocess.STDOUT)
        tracy = None
        if i == 1 and a.tracy_seconds > 0:
            tracy = subprocess.Popen([str(TRACY / "tracy-capture.exe"), "-o", str(out / "trace.tracy"), "-a",
                                      "127.0.0.1", "-p", "8086", "-f", "-s", str(int(a.tracy_seconds))],
                                     stdout=open(out / "tracy-capture.log", "w"), stderr=subprocess.STDOUT)
        t_win = time.perf_counter() - t_launch
        q0 = qpc_s()
        ft = subprocess.run([sys.executable, str(wdir / "capture_frametimes.py"), str(pid), str(a.window_s),
                             str(wdir), "--label", f"{a.label} window {i}"], capture_output=True, text=True)
        q1 = qpc_s()
        if gpu:
            gpu.wait(timeout=a.window_s + 60)
        if tracy:
            tracy.wait(timeout=a.tracy_seconds + 120)
            exports = [("tracy-zones.csv", []), ("tracy-zones-self.csv", ["-e"]), ("tracy-gpu.csv", ["-g"])]
            if a.tracy_events:
                exports.append(("tracy-events.csv", ["-u"]))
            for name, extra in exports:
                with open(out / name, "w") as f:
                    subprocess.run([str(TRACY / "tracy-csvexport.exe")] + extra + [str(out / "trace.tracy")],
                                   stdout=f, stderr=subprocess.STDOUT)
            log("tracy trace exported")
        summ = json.loads((wdir / "frametime-summary.json").read_text()) if (wdir / "frametime-summary.json").exists() else {}
        summ["t_since_launch_s"] = t_win
        summ["qpc_window"] = [q0, q1]
        hwnd_after, _ = find_window(pid)
        summ["client_size_before"] = size_before
        summ["client_size_after"] = client_size(hwnd_after) if hwnd_after else None
        summ["client_size_changed_during_window"] = summ["client_size_before"] != summ["client_size_after"]
        windows.append(summ)
        log(f"window {i}: fps={summ.get('fps_frame_counter')} mean_ms={summ.get('frame_ms_mean')} "
            f"p99_ms={summ.get('frame_ms_p99')} hard_faults={summ.get('emu_hard_faults_total')} "
            f"avail_mib={summ.get('sys_available_mib_mean')} size={size_before}->{summ['client_size_after']}")
        poll_title()
        results["screenshots"].append(screenshot(f"shot-{i}-after.png"))
    results["windows"] = windows
finally:
    results["first_frame_counter_s"] = first_frame_t
    results["key_log"] = key_log  # (s since launch, QPC s, vk, down) for aligning with frames-emu.csv
    results["exited_early"] = proc.poll() is not None
    if not a.keep_running and proc.poll() is None:
        hwnd, _ = find_window(pid)
        if hwnd:
            user32.EnableWindow(hwnd, True)
            user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
        try:
            proc.wait(timeout=45)
            log(f"closed via WM_CLOSE, exit code {proc.returncode}")
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            log("did not exit after WM_CLOSE; terminated (test instance only)")
    results["exit_code"] = proc.returncode
    stdout_f.close()
    stderr_f.close()
    timeline.close()
    text = (out / "emulator-stdout.log").read_text(errors="replace") + (out / "emulator-stderr.log").read_text(errors="replace")
    results["log_markers"] = dict(
        patches_applied=re.findall(r"Successfully applied cheat: (.*)", text),
        pipeline_cache=re.findall(r"Vulkan pipeline cache: .*", text),
        present_mode=re.findall(r".*present mode.*", text)[:3],
        unhandled=re.findall(r".*Unhandled host exception.*", text)[:5],
        asserts=re.findall(r".*ASSERT.*", text)[:5],
        spirv_validation_failed=len(re.findall(r"SPIR-V validation failed", text)),
        errorish_lines=len(re.findall(r"(?im)^.*\b(error|failed|fatal)\b.*$", text)),
    )
    frame_log = out / "frames-emu.csv"
    if frame_log.exists():
        sys.path.insert(0, str(TOOLS))
        import frame_log as fl
        try:
            for summ in results.get("windows", []):
                if "qpc_window" in summ:
                    summ["emu_frames"] = fl.summarize(frame_log, *summ["qpc_window"])
            results["emu_frames_whole_run"] = fl.summarize(frame_log)
            for i, summ in enumerate(results.get("windows", []), 1):
                ef = summ.get("emu_frames") or {}
                if ef.get("valid"):
                    log(f"window {i} (emulator frame log): fps={ef['fps']:.2f} "
                        f"p50={ef['frame_ms']['p50']:.1f} p99={ef['frame_ms']['p99']:.1f} "
                        f"1%low={ef['low_1pct_fps']:.1f} repeats={ef['repeats']}")
        except Exception as e:  # keep the run's other results
            results["emu_frames_error"] = repr(e)
    if save_dir.exists():
        results["post_saves"] = {str(p.relative_to(cwd)): sha256(p) for p in save_dir.rglob("*") if p.is_file()}
    results["post_memory"] = memstat()
    (out / "run-results.json").write_text(json.dumps(results, indent=2))
    log("done")
