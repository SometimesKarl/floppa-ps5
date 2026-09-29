"""Describe large committed host-private blocks of a process: protection, and a content sample.

Usage: inspect_blocks.py PID [MIN_MIB]
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import sys


class MBI(c.Structure):
    _fields_ = [("BaseAddress", c.c_void_p), ("AllocationBase", c.c_void_p),
                ("AllocationProtect", w.DWORD), ("PartitionId", w.WORD), ("RegionSize", c.c_size_t),
                ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]


k = c.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MBI), c.c_size_t]
k.VirtualQueryEx.restype = c.c_size_t
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]

pid = int(sys.argv[1])
min_size = int(sys.argv[2] if len(sys.argv) > 2 else 64) << 20
h = k.OpenProcess(0x0400 | 0x0010, False, pid)
if not h:
    raise SystemExit(f"OpenProcess failed: {c.get_last_error()}")

allocs = collections.OrderedDict()
addr, mbi = 0, MBI()
while addr < 0x7FFFFFFF0000 and k.VirtualQueryEx(h, c.c_void_p(addr), c.byref(mbi), c.sizeof(mbi)):
    base, size = mbi.BaseAddress or 0, mbi.RegionSize
    if mbi.State == 0x1000 and mbi.Type == 0x20000 and base >= (1 << 40):
        a = allocs.setdefault(mbi.AllocationBase or 0, {"committed": 0, "protect": set(), "alloc_protect": mbi.AllocationProtect})
        a["committed"] += size
        a["protect"].add(mbi.Protect)
    addr = base + size


def sample(base, offset, n=64):
    buf = (c.c_ubyte * n)()
    got = c.c_size_t()
    if k.ReadProcessMemory(h, c.c_void_p(base + offset), buf, n, c.byref(got)) and got.value:
        return bytes(buf[:got.value])
    return None


for base, a in allocs.items():
    if a["committed"] < min_size:
        continue
    prots = ",".join(hex(p) for p in sorted(a["protect"]))
    s0 = sample(base, 0)
    mid = sample(base, a["committed"] // 2)
    nz = lambda b: None if b is None else sum(1 for x in b if x) / len(b)
    print(f"0x{base:012x} {a['committed'] >> 20:6d} MiB protect={prots} alloc_protect={hex(a['alloc_protect'])} "
          f"nonzero(start)={nz(s0)} nonzero(mid)={nz(mid)} head={s0[:16].hex() if s0 else None}")
k.CloseHandle(h)
