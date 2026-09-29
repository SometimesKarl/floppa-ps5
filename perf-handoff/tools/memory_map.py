"""Committed memory of a process by region type and address range (a small VMMap).

Usage: memory_map.py PID [SECONDS_BETWEEN [SAMPLES]]
Prints committed MiB per bucket: guest-range private/mapped (below 1 TiB, where the PS5 guest
address space lives in KytyPS5) versus host-range private/mapped/image (heaps, drivers, DLLs).
With several samples it also prints the change per second of each bucket.
"""
import ctypes as c
import ctypes.wintypes as w
import sys
import time


class MBI(c.Structure):
    _fields_ = [("BaseAddress", c.c_void_p), ("AllocationBase", c.c_void_p),
                ("AllocationProtect", w.DWORD), ("PartitionId", w.WORD), ("RegionSize", c.c_size_t),
                ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]


k = c.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MBI), c.c_size_t]
k.VirtualQueryEx.restype = c.c_size_t

MEM_COMMIT, MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE = 0x1000, 0x20000, 0x40000, 0x1000000
GUEST_LIMIT = 1 << 40  # 1 TiB


def sample(pid):
    h = k.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
    if not h:
        raise SystemExit(f"OpenProcess failed: {c.get_last_error()}")
    buckets = {}
    big = {}
    addr = 0
    mbi = MBI()
    while addr < 0x7FFFFFFF0000:
        if k.VirtualQueryEx(h, c.c_void_p(addr), c.byref(mbi), c.sizeof(mbi)) == 0:
            break
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT:
            kind = {MEM_PRIVATE: "private", MEM_MAPPED: "mapped", MEM_IMAGE: "image"}.get(mbi.Type, "other")
            where = "guest" if base < GUEST_LIMIT else "host"
            key = f"{where}-{kind}"
            buckets[key] = buckets.get(key, 0) + size
            alloc = mbi.AllocationBase or 0
            big[(key, alloc)] = big.get((key, alloc), 0) + size
        addr = base + size
    k.CloseHandle(h)
    return buckets, big


pid = int(sys.argv[1])
gap = float(sys.argv[2]) if len(sys.argv) > 2 else 0
count = int(sys.argv[3]) if len(sys.argv) > 3 else (2 if gap else 1)
samples = []
for i in range(count):
    t = time.perf_counter()
    samples.append((t, *sample(pid)))
    if i + 1 < count:
        time.sleep(gap)

t0, first, first_big = samples[0]
print("bucket              committed MiB" + ("   change MiB/s" if count > 1 else ""))
t1, last, last_big = samples[-1]
for key in sorted(set(first) | set(last)):
    line = f"{key:18s} {last.get(key, 0) / 2**20:12.0f}"
    if count > 1:
        line += f" {(last.get(key, 0) - first.get(key, 0)) / 2**20 / (t1 - t0):14.2f}"
    print(line)
if count > 1:
    print("fastest-growing allocations (MiB/s):")
    growth = sorted(((last_big.get(kk, 0) - first_big.get(kk, 0), kk) for kk in set(first_big) | set(last_big)),
                    reverse=True)[:8]
    for d, (key, alloc) in growth:
        if d > 0:
            print(f"  {key:18s} base 0x{alloc:012x} +{d / 2**20 / (t1 - t0):.2f} MiB/s "
                  f"(now {last_big.get((key, alloc), 0) / 2**20:.0f} MiB)")

print("largest host-private allocations (MiB):")
for (key, alloc), size in sorted(last_big.items(), key=lambda kv: -kv[1])[:40]:
    if key == "host-private" and size >= 16 * 2**20:
        print(f"  base 0x{alloc:012x} {size / 2**20:8.0f}")
