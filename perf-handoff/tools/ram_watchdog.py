"""Stops the TEST emulator when the PC runs low on memory, so a test cannot freeze the machine.

Usage: ram_watchdog.py [MIN_FREE_MIB=1536] [LOG_FILE]

Reads the test process identity (pid + creation time + executable path) that tools/drive.py
recorded at launch, once, at start: a later launch rewriting drive-state.json does not retarget a
running watchdog. Every 0.5 s it checks available physical memory and commit headroom; when either
stays under MIN_FREE_MIB for 2 s it terminates that process, only if it is still the same process
(owned_process.terminate). It never looks processes up by name: the user may be playing in their
own emulator at the same time. It exits when the test process exits.
"""
import ctypes as c
import ctypes.wintypes as w
import json
import sys
import time
from pathlib import Path

import owned_process

STATE = Path(__file__).resolve().parent / "drive-state.json"


class MEMORYSTATUSEX(c.Structure):
    _fields_ = [("dwLength", w.DWORD), ("dwMemoryLoad", w.DWORD), ("ullTotalPhys", c.c_ulonglong),
                ("ullAvailPhys", c.c_ulonglong), ("ullTotalPageFile", c.c_ulonglong),
                ("ullAvailPageFile", c.c_ulonglong), ("ullTotalVirtual", c.c_ulonglong),
                ("ullAvailVirtual", c.c_ulonglong), ("ullAvailExtendedVirtual", c.c_ulonglong)]


def headroom_mib():
    """(available physical MiB, available commit MiB)."""
    status = MEMORYSTATUSEX()
    status.dwLength = c.sizeof(status)
    c.windll.kernel32.GlobalMemoryStatusEx(c.byref(status))
    return status.ullAvailPhys / 1048576, status.ullAvailPageFile / 1048576


class PROCESS_MEMORY_COUNTERS_EX(c.Structure):
    _fields_ = [("cb", w.DWORD), ("PageFaultCount", w.DWORD), ("PeakWorkingSetSize", c.c_size_t),
                ("WorkingSetSize", c.c_size_t), ("QuotaPeakPagedPoolUsage", c.c_size_t),
                ("QuotaPagedPoolUsage", c.c_size_t), ("QuotaPeakNonPagedPoolUsage", c.c_size_t),
                ("QuotaNonPagedPoolUsage", c.c_size_t), ("PagefileUsage", c.c_size_t),
                ("PeakPagefileUsage", c.c_size_t), ("PrivateUsage", c.c_size_t)]


def process_mib(pid):
    """(working set MiB, peak working set MiB, private commit MiB) of the process, or (0, 0, 0)."""
    k = c.windll.kernel32
    k.OpenProcess.restype = w.HANDLE
    handle = k.OpenProcess(0x1000 | 0x0010, False, pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    if not handle:
        return 0, 0, 0
    try:
        counters = PROCESS_MEMORY_COUNTERS_EX()
        counters.cb = c.sizeof(counters)
        if not c.windll.psapi.GetProcessMemoryInfo(w.HANDLE(handle), c.byref(counters), counters.cb):
            return 0, 0, 0
        return (counters.WorkingSetSize / 1048576, counters.PeakWorkingSetSize / 1048576,
                counters.PrivateUsage / 1048576)
    finally:
        k.CloseHandle(w.HANDLE(handle))


def log_line(log, text):
    line = f"{time.strftime('%H:%M:%S')} {text}"
    print(line, flush=True)
    if log:
        with open(log, "a") as f:
            f.write(line + "\n")


def main():
    limit = float(sys.argv[1]) if len(sys.argv) > 1 else 1536
    log = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    owned = json.loads(STATE.read_text()).get("owned")
    if not owned or not owned_process.is_same(owned):
        log_line(log, "watchdog: no live test process recorded in drive-state.json; exiting")
        return
    log_line(log, f"watchdog: guarding pid {owned['pid']} ({owned['exe']}), limit {limit:.0f} MiB")
    low_since = None
    last_report = 0.0
    while owned_process.is_same(owned):
        free, commit = headroom_mib()
        if time.time() - last_report >= 10:
            last_report = time.time()
            ws, peak, private = process_mib(owned["pid"])
            log_line(log, f"watchdog: free RAM {free:.0f} MiB, test working set {ws:.0f} MiB "
                          f"(peak {peak:.0f}, private {private:.0f})")
        if min(free, commit) < limit:
            if not low_since:
                # Who took the memory: the test process or something else on the PC.
                ws, peak, private = process_mib(owned["pid"])
                log_line(log, f"watchdog: low: free RAM {free:.0f} MiB, commit headroom {commit:.0f} MiB, "
                              f"test working set {ws:.0f} MiB (peak {peak:.0f}, private {private:.0f})")
            low_since = low_since or time.time()
            if time.time() - low_since >= 2:
                killed = owned_process.terminate(owned)
                log_line(log, f"watchdog: free RAM {free:.0f} MiB, commit headroom {commit:.0f} MiB "
                              f"< {limit:.0f}: {'terminated' if killed else 'could not terminate'} "
                              f"test pid {owned['pid']}")
                return
        else:
            low_since = None
        time.sleep(0.5)
    log_line(log, f"watchdog: test pid {owned['pid']} exited")


main()
