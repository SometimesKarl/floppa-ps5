"""Drive one emulator instance step by step (for learning input routes by hand).

  drive.py launch EXE OUT_DIR [--cwd-template DIR] [extra emulator args...]
  drive.py keys SPEC [SPEC ...]      SPEC = KEY[:HOLD_S[:GAP_S]], e.g. RETURN DOWN J  or  W:2.0
  drive.py shot NAME                 screenshot (PrintWindow) into OUT_DIR, prints luma
  drive.py status                    seconds since launch, window title
  drive.py close                     WM_CLOSE to the test window
  drive.py kill                      terminate the test process (only if it is still the one launched)

Launches with the launcher's recorded argument list (like run_experiment.py) in a fresh copy of
the testbed saves; state lives in drive-state.json next to this script.
"""
import ctypes as c
import ctypes.wintypes as w
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import owned_process

TOOLS = Path(__file__).resolve().parent
STATE = TOOLS / "drive-state.json"
HANDOFF_LAUNCH = Path(r"C:\Users\himav\Desktop\kyty ps5\Performance Handoff\system-and-launch.json")
TEMPLATE = Path(os.environ.get("DRIVE_TEMPLATE", TOOLS.parent / "testbed" / "installed-exe"))
user32 = c.WinDLL("user32", use_last_error=True)
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.MapVirtualKeyW.argtypes = [w.UINT, w.UINT]
user32.MapVirtualKeyW.restype = w.UINT
VK_NAMES = {"RETURN": 0x0D, "SPACE": 0x20, "ESCAPE": 0x1B, "LEFT": 0x25, "UP": 0x26, "RIGHT": 0x27,
            "DOWN": 0x28}


def find_window(pid):
    found = []
    proto = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)

    def cb(hwnd, _):
        p = w.DWORD()
        user32.GetWindowThreadProcessId(hwnd, c.byref(p))
        if p.value == pid and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = c.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if "fps" in buf.value or "ASTRO" in buf.value:
                found.append((hwnd, buf.value))
        return True

    user32.EnumWindows(proto(cb), 0)
    return found[0] if found else (None, None)


def post_key(pid, vk, down):
    hwnd, _ = find_window(pid)
    if not hwnd:
        print("no window")
        return
    scan = user32.MapVirtualKeyW(vk, 0)
    if down:
        user32.PostMessageW(hwnd, 0x0100, vk, 1 | (scan << 16))
    else:
        user32.PostMessageW(hwnd, 0x0101, vk, 1 | (scan << 16) | (1 << 30) | (1 << 31))


def main():
    cmd = sys.argv[1]
    if cmd == "launch":
        exe, out = Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve()
        extra = sys.argv[4:]
        out.mkdir(parents=True, exist_ok=False)
        cwd = out / "cwd"
        shutil.copytree(TEMPLATE, cwd)
        launch = json.loads(HANDOFF_LAUNCH.read_text(encoding="utf-8-sig"))
        args = re.findall(r'"([^"]*)"', launch["Process"]["CommandLine"])[1:] + extra
        # DRIVE_GAME: another eboot.bin (e.g. Demon's Souls); DRIVE_PATCH: another game patch,
        # or "none" for none (the recorded ones are ASTRO BOT's).
        if os.environ.get("DRIVE_GAME"):
            args[args.index("--game") + 1] = os.environ["DRIVE_GAME"]
        if os.environ.get("DRIVE_PATCH"):
            i = args.index("--game-patch")
            if os.environ["DRIVE_PATCH"] == "none":
                del args[i:i + 2]
            else:
                args[i + 1] = os.environ["DRIVE_PATCH"]
        # DRIVE_NO_VALIDATION=1: shader/Vulkan validation off (the recorded launch has them on;
        # earlier baselines were measured with them on, so keep off only for new comparisons).
        if os.environ.get("DRIVE_NO_VALIDATION") == "1":
            for flag in ("--shader-validation", "--vulkan-validation"):
                args[args.index(flag) + 1] = "false"
        env = os.environ.copy()
        for hint in ("SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI",
                     "SDL_JOYSTICK_DIRECTINPUT", "SDL_JOYSTICK_GAMEINPUT", "SDL_XINPUT_ENABLED"):
            env.setdefault(hint, "0")
        env.setdefault("KYTY_FRAME_LOG", str(out / "frames-emu.csv"))
        proc = subprocess.Popen([str(exe)] + args, cwd=str(cwd), env=env,
                                stdout=open(out / "emulator-stdout.log", "wb"),
                                stderr=open(out / "emulator-stderr.log", "wb"),
                                creationflags=0x08000000 | 0x00000008)  # NO_WINDOW | DETACHED
        owned = None
        for _ in range(50):
            owned = owned_process.identity(proc.pid)
            if owned:
                break
            time.sleep(0.1)
        STATE.write_text(json.dumps(dict(pid=proc.pid, t0=time.time(), out=str(out), owned=owned)))
        print(f"pid {proc.pid}")
        return
    st = json.loads(STATE.read_text())
    pid, out = st["pid"], Path(st["out"])
    if cmd in ("keys", "shot") and not (st.get("owned") and owned_process.is_same(st["owned"])):
        print("test process not running")
        return
    t = time.time() - st["t0"]
    if cmd == "keys":
        for spec in sys.argv[2:]:
            parts = spec.split(":")
            key = parts[0].upper()
            hold = float(parts[1]) if len(parts) > 1 else 0.15
            gap = float(parts[2]) if len(parts) > 2 else 0.6
            vk = VK_NAMES.get(key, ord(key) if len(key) == 1 else None)
            post_key(pid, vk, True)
            time.sleep(hold)
            post_key(pid, vk, False)
            with open(out / "keys.log", "a") as f:
                f.write(f"{time.time() - st['t0']:.2f} {key} {hold}\n")
            time.sleep(gap)
        print(f"keys sent at {t:.1f}s")
    elif cmd == "shot":
        r = subprocess.run([sys.executable, str(TOOLS / "capture_window.py"), str(pid),
                            str(out / (sys.argv[2] + ".png"))], capture_output=True, text=True)
        try:
            d = json.loads(r.stdout)
            print(f"{t:.1f}s luma={d['printwindow']['mean_luma']:.1f} {d['title'][-30:]}")
        except Exception:
            print(r.stdout, r.stderr)
    elif cmd == "status":
        print(f"{t:.1f}s", find_window(pid)[1])
    elif cmd == "close":
        if not st.get("owned") or not owned_process.is_same(st["owned"]):
            print("test process not running")
            return
        hwnd, _ = find_window(pid)
        if hwnd:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        print("close posted")
    elif cmd == "kill":
        ok = bool(st.get("owned")) and owned_process.terminate(st["owned"])
        print("terminated" if ok else "not terminated (not running, or no longer the launched process)")


main()
