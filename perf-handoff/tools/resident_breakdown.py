"""Where a process's resident RAM is: guest range vs host, private vs mapped, shared or not.

Usage: resident_breakdown.py PID
Reads the whole working set (QueryWorkingSet) and buckets each resident page by the region
type around it (VirtualQueryEx). Guest memory lives below 1 TiB in KytyPS5. Pages mapped twice
in the process (for example a guest view and a backing view of one section) are counted per
view, so "mapped" can exceed the physical RAM those pages use; sharecount shows it.
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import sys

k = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
k.OpenProcess.restype = w.HANDLE


class MBI(c.Structure):
    _fields_ = [("BaseAddress", c.c_void_p), ("AllocationBase", c.c_void_p),
                ("AllocationProtect", w.DWORD), ("PartitionId", w.WORD), ("RegionSize", c.c_size_t),
                ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD)]


pid = int(sys.argv[1])
h = k.OpenProcess(0x0400 | 0x0010, False, pid)  # QUERY_INFORMATION | VM_READ
if not h:
    sys.exit(f"OpenProcess failed: {c.get_last_error()}")

# Regions (committed only), sorted by base.
regions = []
address = 0
mbi = MBI()
while k.VirtualQueryEx(h, c.c_void_p(address), c.byref(mbi), c.sizeof(mbi)):
    base = mbi.BaseAddress or 0
    if mbi.State == 0x1000:  # MEM_COMMIT
        regions.append((base, base + mbi.RegionSize, mbi.Type))
    address = base + mbi.RegionSize
    if address >= 0x7FFFFFFF0000:
        break

# Working set: entries are ULONG_PTR; [0] = count.
size = 1 << 22
while True:
    buf = (c.c_size_t * size)()
    if psapi.QueryWorkingSet(h, buf, c.sizeof(buf)):
        break
    if c.get_last_error() != 24:  # ERROR_BAD_LENGTH
        sys.exit(f"QueryWorkingSet failed: {c.get_last_error()}")
    size = int(buf[0]) + (1 << 20)
count = int(buf[0])
pages = sorted((buf[i + 1] & ~0xFFF, buf[i + 1] & 0xFFF) for i in range(count))

TYPE = {0x20000: "private", 0x40000: "mapped", 0x1000000: "image"}
buckets = collections.Counter()
shared = collections.Counter()
j = 0
for va, flags in pages:
    while j < len(regions) and regions[j][1] <= va:
        j += 1
    kind = "?"
    if j < len(regions) and regions[j][0] <= va:
        kind = TYPE.get(regions[j][2], "?")
    where = "guest" if va < (1 << 40) else "host"
    key = f"{where}-{kind}"
    buckets[key] += 1
    if flags & 0x100:  # Shared bit
        shared[key] += 1
total = sum(buckets.values())
print(f"working set {total * 4 / 1024:.0f} MiB ({total} pages)")
for key, n in buckets.most_common():
    print(f"  {key:16s} {n * 4 / 1024:8.0f} MiB   shared {shared[key] * 4 / 1024:8.0f} MiB")
