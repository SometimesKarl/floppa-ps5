"""Sample where threads of a process execute (instruction pointer histogram by module+offset).

Usage: thread_sample.py PID NAME_SUBSTRING [SAMPLES=200] [INTERVAL_MS=20] [DUMP_FILE]
Suspends each matching thread for a moment to read its context (x64). Addresses outside any
module are reported as 'guest/anonymous' with the 64 KiB page, which for KytyPS5 is guest code.
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import sys
import time
from pathlib import Path

k = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
k.OpenThread.restype = w.HANDLE
k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.CreateToolhelp32Snapshot.restype = w.HANDLE
k.GetThreadDescription.argtypes = [w.HANDLE, c.POINTER(c.c_wchar_p)]
k.SuspendThread.argtypes = [w.HANDLE]
k.ResumeThread.argtypes = [w.HANDLE]
k.GetThreadContext.argtypes = [w.HANDLE, c.c_void_p]
k.CloseHandle.argtypes = [w.HANDLE]
psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]


class THREADENTRY32(c.Structure):
    _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ThreadID", w.DWORD),
                ("th32OwnerProcessID", w.DWORD), ("tpBasePri", w.LONG), ("tpDeltaPri", w.LONG),
                ("dwFlags", w.DWORD)]


class CONTEXT(c.Structure):  # x64 CONTEXT, 16-byte aligned via padding in the buffer below
    _fields_ = [("P1Home", c.c_uint64), ("P2Home", c.c_uint64), ("P3Home", c.c_uint64),
                ("P4Home", c.c_uint64), ("P5Home", c.c_uint64), ("P6Home", c.c_uint64),
                ("ContextFlags", w.DWORD), ("MxCsr", w.DWORD), ("SegCs", w.WORD), ("SegDs", w.WORD),
                ("SegEs", w.WORD), ("SegFs", w.WORD), ("SegGs", w.WORD), ("SegSs", w.WORD),
                ("EFlags", w.DWORD), ("Dr0", c.c_uint64), ("Dr1", c.c_uint64), ("Dr2", c.c_uint64),
                ("Dr3", c.c_uint64), ("Dr6", c.c_uint64), ("Dr7", c.c_uint64), ("Rax", c.c_uint64),
                ("Rcx", c.c_uint64), ("Rdx", c.c_uint64), ("Rbx", c.c_uint64), ("Rsp", c.c_uint64),
                ("Rbp", c.c_uint64), ("Rsi", c.c_uint64), ("Rdi", c.c_uint64), ("R8", c.c_uint64),
                ("R9", c.c_uint64), ("R10", c.c_uint64), ("R11", c.c_uint64), ("R12", c.c_uint64),
                ("R13", c.c_uint64), ("R14", c.c_uint64), ("R15", c.c_uint64), ("Rip", c.c_uint64),
                ("Rest", c.c_byte * 1024)]


class MODINFO(c.Structure):
    _fields_ = [("base", c.c_void_p), ("size", w.DWORD), ("entry", c.c_void_p)]


psapi.GetModuleInformation.argtypes = [w.HANDLE, w.HMODULE, c.POINTER(MODINFO), w.DWORD]


pid = int(sys.argv[1])
pattern = sys.argv[2]
samples = int(sys.argv[3]) if len(sys.argv) > 3 else 200
interval = (int(sys.argv[4]) if len(sys.argv) > 4 else 20) / 1000
EXACT = __import__("os").environ.get("THREAD_SAMPLE_EXACT") == "1"

hp = k.OpenProcess(0x0400 | 0x0010, False, pid)
arr = (w.HMODULE * 4096)()
needed = w.DWORD()
psapi.EnumProcessModulesEx(hp, arr, c.sizeof(arr), c.byref(needed), 3)
mods = []
for i in range(needed.value // c.sizeof(w.HMODULE)):
    name = c.create_unicode_buffer(512)
    psapi.GetModuleFileNameExW(hp, arr[i], name, 512)
    info = MODINFO()
    psapi.GetModuleInformation(hp, arr[i], c.byref(info), c.sizeof(info))
    mods.append((info.base or 0, info.size, Path(name.value).name))


def where(addr):
    for base, size, name in mods:
        if base <= addr < base + size:
            return f"{name}+0x{addr - base:x}"
    # THREAD_SAMPLE_EXACT=1: exact guest addresses (for guest_disasm.py) instead of pages.
    return f"anon@0x{addr if EXACT else addr & ~0xfff:x}"


snap = k.CreateToolhelp32Snapshot(0x4, 0)
e = THREADENTRY32()
e.dwSize = c.sizeof(e)
tids = []
ok = k.Thread32First(snap, c.byref(e))
while ok:
    if e.th32OwnerProcessID == pid:
        tids.append(e.th32ThreadID)
    ok = k.Thread32Next(snap, c.byref(e))
handles = []
for tid in tids:
    h = k.OpenThread(0x0002 | 0x0008 | 0x0040, False, tid)  # SUSPEND_RESUME | GET_CONTEXT | QUERY
    if not h:
        continue
    nm = c.c_wchar_p()
    exact = pattern.startswith("=")
    if k.GetThreadDescription(h, c.byref(nm)) >= 0 and nm.value and (
            nm.value == pattern[1:] if exact else pattern in nm.value):
        handles.append((h, nm.value))
    else:
        k.CloseHandle(h)
print(f"sampling {len(handles)} threads matching '{pattern}'")
buf = (c.c_byte * (c.sizeof(CONTEXT) + 16))()
addr = (c.addressof(buf) + 15) & ~15
ctx = CONTEXT.from_address(addr)
hist = collections.Counter()
for _ in range(samples):
    for h, nm in handles:
        if k.SuspendThread(h) == 0xFFFFFFFF:
            continue
        ctx.ContextFlags = 0x00100001  # CONTEXT_AMD64 | CONTEXT_CONTROL
        if k.GetThreadContext(h, c.c_void_p(addr)):
            hist[where(ctx.Rip)] += 1
        k.ResumeThread(h)
    time.sleep(interval)
total = sum(hist.values())
for loc, n in hist.most_common(25):
    print(f"{100 * n / total:5.1f}%  {loc}")
if len(sys.argv) > 5:
    # Every sample location with its count, for symbolize_samples.py.
    with open(sys.argv[5], "w") as f:
        for loc, n in hist.most_common():
            f.write(f"{n} {loc}\n")
