"""Are a process's large host-private blocks resident RAM? Samples pages with QueryWorkingSetEx.

Usage: block_residency.py PID [MIN_MIB]
For each committed private allocation of at least MIN_MIB (default 64): protection, and the
fraction of sampled pages that are valid in the working set. Pages of a device-memory aperture
(for example CPU-mapped VRAM) are not RAM and are typically not reported as working-set pages.
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import sys


class MBI(c.Structure):
    _fields_ = [("BaseAddress", c.c_void_p), ("AllocationBase", c.c_void_p),
                ("AllocationProtect", w.DWORD), ("PartitionId", w.WORD), ("RegionSize", c.c_size_t),
                ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]


class WSEX(c.Structure):
    _fields_ = [("VirtualAddress", c.c_void_p), ("VirtualAttributes", c.c_size_t)]


k = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MBI), c.c_size_t]
k.VirtualQueryEx.restype = c.c_size_t
psapi.QueryWorkingSetEx.argtypes = [w.HANDLE, c.c_void_p, w.DWORD]
psapi.QueryWorkingSetEx.restype = w.BOOL

pid = int(sys.argv[1])
min_size = int(sys.argv[2] if len(sys.argv) > 2 else 64) << 20
h = k.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
if not h:
    raise SystemExit(f"OpenProcess failed: {c.get_last_error()}")

allocs = collections.OrderedDict()
addr = 0
mbi = MBI()
while k.VirtualQueryEx(h, c.c_void_p(addr), c.byref(mbi), c.sizeof(mbi)):
    base = mbi.BaseAddress or 0
    if mbi.State == 0x1000 and mbi.Type == 0x20000 and base >= (1 << 40):  # MEM_COMMIT, MEM_PRIVATE, host range
        a = allocs.setdefault(mbi.AllocationBase, dict(size=0, protect=collections.Counter(), regions=[]))
        a["size"] += mbi.RegionSize
        a["protect"][hex(mbi.Protect)] += mbi.RegionSize
        a["regions"].append((base, mbi.RegionSize))
    addr = base + mbi.RegionSize
    if addr >= (1 << 47):
        break

SAMPLES = 256
totals = collections.Counter()
print(f"{'allocation base':>16s} {'MiB':>6s} {'protect':>10s} {'resident %':>10s}")
for ab, a in allocs.items():
    if a["size"] < min_size:
        continue
    pages = []
    for base, size in a["regions"]:
        n = max(1, int(SAMPLES * size / a["size"]))
        step = max(4096, (size // n) & ~4095)
        pages += [base + i * step for i in range(n) if i * step < size]
    arr = (WSEX * len(pages))(*[WSEX(p, 0) for p in pages])
    if not psapi.QueryWorkingSetEx(h, arr, c.sizeof(arr)):
        print(f"QueryWorkingSetEx failed: {c.get_last_error()}")
        break
    valid = sum(1 for e in arr if e.VirtualAttributes & 1)
    frac = valid / len(pages)
    prot = ",".join(p for p, _ in a["protect"].most_common(2))
    totals[prot] += a["size"]
    totals[prot + " resident"] += a["size"] * frac
    print(f"0x{ab:014x} {a['size'] >> 20:6d} {prot:>10s} {100 * frac:9.0f}%")
print("totals (MiB):", {kk: round(v / 2**20) for kk, v in totals.items()})
