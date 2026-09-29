"""Which address ranges a process's working set grows in.

Usage: ws_growth.py PID SECONDS [GRANULE_MIB=16]
Takes the working set page list twice, SECONDS apart, and prints the ranges (GRANULE_MIB
buckets) that gained the most resident pages, plus the totals gained and lost.
"""
import collections
import ctypes as c
import sys
import time

k = c.WinDLL("kernel32", use_last_error=True)
psapi = c.WinDLL("psapi", use_last_error=True)
k.OpenProcess.restype = c.c_void_p


def pages(h):
    size = 1 << 22
    while True:
        buf = (c.c_size_t * size)()
        if psapi.QueryWorkingSet(c.c_void_p(h), buf, c.sizeof(buf)):
            break
        size = int(buf[0]) + (1 << 20)
    return {buf[i + 1] & ~0xFFF for i in range(int(buf[0]))}


pid, seconds = int(sys.argv[1]), float(sys.argv[2])
granule = (int(sys.argv[3]) if len(sys.argv) > 3 else 16) << 20
h = k.OpenProcess(0x0400 | 0x0010, False, pid)
a = pages(h)
time.sleep(seconds)
b = pages(h)
gained, lost = b - a, a - b
print(f"gained {len(gained) * 4 / 1024:.0f} MiB, lost {len(lost) * 4 / 1024:.0f} MiB in {seconds:.0f} s")
by = collections.Counter(p // granule * granule for p in gained)
for base, n in by.most_common(20):
    print(f"  0x{base:012x}  +{n * 4 / 1024:6.1f} MiB  ({'guest' if base < (1 << 40) else 'host'})")
