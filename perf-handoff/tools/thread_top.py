"""Per-thread CPU of a process over an interval, with thread names (like top -H).

Usage: thread_top.py PID [SECONDS=5] [TOP=15]
Read-only: THREAD_QUERY_LIMITED_INFORMATION for times, THREAD_QUERY_INFORMATION for names.
"""
import ctypes as c
import ctypes.wintypes as w
import sys
import time

k = c.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPTHREAD = 0x4


class THREADENTRY32(c.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG),
                ("dwFlags", w.DWORD)]


k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.OpenThread.restype = w.HANDLE
k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.GetThreadTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
k.GetThreadDescription.argtypes = [w.HANDLE, c.POINTER(c.c_wchar_p)]


def threads(pid):
    snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = c.sizeof(e)
    out = []
    ok = k.Thread32First(snap, c.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k.Thread32Next(snap, c.byref(e))
    k.CloseHandle(snap)
    return out


def sample(pid):
    res = {}
    for tid in threads(pid):
        h = k.OpenThread(0x0800 | 0x0040, False, tid)
        if not h:
            h = k.OpenThread(0x0800, False, tid)
        if not h:
            continue
        ct, et, kt, ut = w.FILETIME(), w.FILETIME(), w.FILETIME(), w.FILETIME()
        if k.GetThreadTimes(h, c.byref(ct), c.byref(et), c.byref(kt), c.byref(ut)):
            t = ((kt.dwHighDateTime << 32) | kt.dwLowDateTime) + ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)
            name = c.c_wchar_p()
            nm = ""
            if k.GetThreadDescription(h, c.byref(name)) >= 0 and name.value:
                nm = name.value
            res[tid] = (t, nm)
        k.CloseHandle(h)
    return res


pid = int(sys.argv[1])
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
top = int(sys.argv[3]) if len(sys.argv) > 3 else 15
a = sample(pid)
t0 = time.perf_counter()
time.sleep(secs)
b = sample(pid)
dt = time.perf_counter() - t0
rows = []
for tid, (t, nm) in b.items():
    if tid in a:
        rows.append(((t - a[tid][0]) / 1e7 / dt * 100, tid, nm or a[tid][1]))
rows.sort(reverse=True)
print(f"{len(b)} threads; total {sum(r[0] for r in rows):.0f}% of one core over {dt:.1f}s")
for pct, tid, nm in rows[:top]:
    print(f"{pct:6.1f}%  tid {tid:6d}  {nm}")
