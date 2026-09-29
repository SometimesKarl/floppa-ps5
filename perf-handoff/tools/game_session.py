"""Launch a game on a staged emulator build for interactive testing, with logs and crash mapping.

Usage: game_session.py --exe EXE --game EBOOT --out DIR --cwd DIR [--patch JSON] [--env K=V ...]
The window keeps normal input (the user plays). Records stdout/stderr, the loaded-module map
(base, size, path) every 10 s, and on exit maps the crash pc and stack words printed by the
emulator's fault handler to module+offset (crash-report.txt). Never touches Fixed Build.
"""
import argparse
import ctypes as c
import ctypes.wintypes as w
import datetime
import json
import os
import re
import subprocess
import time
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("--exe", required=True)
ap.add_argument("--game", required=True)
ap.add_argument("--out", required=True)
ap.add_argument("--cwd", required=True)
ap.add_argument("--patch")
ap.add_argument("--env", action="append", default=[])
a = ap.parse_args()
out = Path(a.out)
out.mkdir(parents=True, exist_ok=True)
cwd = Path(a.cwd)
cwd.mkdir(parents=True, exist_ok=True)

args = ["--screen-width", "1280", "--screen-height", "720", "--user-name", "soda", "--user-id", "1000",
        "--present-mode", "Mailbox", "--gpu", "0", "--readback-linear-images", "false",
        "--vblank-frequency", "60", "--console-language", "1", "--vulkan-validation", "false",
        "--shader-validation", "true", "--shader-optimization-type", "Performance",
        "--shader-log-direction", "Silent", "--shader-log-folder", "_Shaders",
        "--command-buffer-dump", "false", "--command-buffer-dump-folder", "_Buffers",
        "--printf-direction", "Silent", "--printf-output-file", "_kyty.txt",
        "--spirv-debug-printf", "false", "--amd-cpu", "--game", a.game]
if a.patch:
    args += ["--game-patch", a.patch]
env = os.environ.copy()
env.setdefault("KYTY_FRAME_LOG", str(out / "frames-emu.csv"))
for kv in a.env:
    k, v = kv.split("=", 1)
    env[k] = v

k32 = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]


class MODINFO(c.Structure):
    _fields_ = [("base", c.c_void_p), ("size", w.DWORD), ("entry", c.c_void_p)]


psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
psapi.GetModuleInformation.argtypes = [w.HANDLE, w.HMODULE, c.POINTER(MODINFO), w.DWORD]


def module_map(pid):
    h = k32.OpenProcess(0x0400 | 0x0010, False, pid)
    if not h:
        return None
    try:
        arr = (w.HMODULE * 4096)()
        needed = w.DWORD()
        if not psapi.EnumProcessModulesEx(h, arr, c.sizeof(arr), c.byref(needed), 0x03):
            return None
        mods = []
        for i in range(needed.value // c.sizeof(w.HMODULE)):
            name = c.create_unicode_buffer(1024)
            psapi.GetModuleFileNameExW(h, arr[i], name, 1024)
            info = MODINFO()
            psapi.GetModuleInformation(h, arr[i], c.byref(info), c.sizeof(info))
            mods.append({"base": info.base or 0, "size": info.size, "path": name.value})
        return mods
    finally:
        k32.CloseHandle(h)


def log(msg):
    line = f"[{datetime.datetime.now().isoformat(timespec='seconds')}] {msg}"
    print(line, flush=True)
    with open(out / "session.log", "a") as f:
        f.write(line + "\n")


meta = dict(exe=a.exe, game=a.game, patch=a.patch, cwd=str(cwd), args=args, env=a.env,
            started=datetime.datetime.now().isoformat())
(out / "session-meta.json").write_text(json.dumps(meta, indent=2))
stdout_f = open(out / "emulator-stdout.log", "wb")
stderr_f = open(out / "emulator-stderr.log", "wb")
t0 = time.time()
proc = subprocess.Popen([a.exe] + args, cwd=str(cwd), env=env, stdout=stdout_f, stderr=stderr_f,
                        creationflags=0x08000000)
log(f"launched pid {proc.pid}: {a.exe}")
mods = None
while proc.poll() is None:
    m = module_map(proc.pid)
    if m:
        mods = m
        (out / "modules.json").write_text(json.dumps(mods, indent=1))
    time.sleep(10)
dur = time.time() - t0
stdout_f.close()
stderr_f.close()
log(f"exited with code {proc.returncode} (0x{proc.returncode & 0xffffffff:08x}) after {dur:.0f}s")

text = (out / "emulator-stdout.log").read_text(errors="replace") + (out / "emulator-stderr.log").read_text(errors="replace")


def where(addr):
    for m in mods or []:
        if m["base"] <= addr < m["base"] + m["size"]:
            return f"{Path(m['path']).name}+0x{addr - m['base']:x}"
    return None


report = []
for m in re.finditer(r"Unhandled host exception:.*?pc=(0x[0-9a-fA-F]+).*?address=(0x[0-9a-fA-F]+)", text):
    pc = int(m.group(1), 16)
    report.append(f"fault pc {m.group(1)} -> {where(pc)} ; access address {m.group(2)}")
stack = re.search(r"stack:\s*\n((?:\s+[0-9a-f]{16}(?:\s+[0-9a-f]{16})*\s*\n)+)", text)
if stack:
    for word in re.findall(r"[0-9a-f]{16}", stack.group(1)):
        v = int(word, 16)
        loc = where(v)
        if loc:
            report.append(f"stack word {word} -> {loc}")
for line in text.splitlines():
    if re.search(r"Error|EXIT|assert|Unhandled|fault|thread:|in C:/", line):
        report.append("log: " + line.strip()[:300])
(out / "crash-report.txt").write_text("\n".join(report) + "\n")
log(f"crash report lines: {len(report)} -> {out / 'crash-report.txt'}")
