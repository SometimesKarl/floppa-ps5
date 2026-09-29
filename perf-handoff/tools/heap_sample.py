"""Dump samples of a committed region of another process: hex, ASCII runs and repeated dwords.

Usage: heap_sample.py PID BASE_HEX [BYTES=65536] [OFFSET_HEX=0]
Read-only (PROCESS_VM_READ). Used to guess what fills a growing heap segment.
"""
import collections
import ctypes as c
import ctypes.wintypes as w
import re
import sys

k = c.WinDLL("kernel32", use_last_error=True)
k.OpenProcess.restype = w.HANDLE
k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]

pid = int(sys.argv[1])
base = int(sys.argv[2], 16)
size = int(sys.argv[3]) if len(sys.argv) > 3 else 65536
offset = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
h = k.OpenProcess(0x0010 | 0x0400, False, pid)
buf = (c.c_ubyte * size)()
got = c.c_size_t()
data = b""
# Read page by page so an uncommitted page does not fail the whole read.
for page in range(0, size, 4096):
    if k.ReadProcessMemory(h, c.c_void_p(base + offset + page), c.byref(buf, page), 4096, c.byref(got)):
        data += bytes(buf[page:page + 4096])
    else:
        data += b"\0" * 4096
print(f"read {len(data)} bytes at 0x{base + offset:x}; nonzero {sum(1 for x in data if x) / len(data):.2f}")
strings = collections.Counter(m.group().decode() for m in re.finditer(rb"[ -~]{6,}", data))
print("ASCII runs:", strings.most_common(25))
qwords = collections.Counter(int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data) - 7, 8))
print("common qwords:", [(hex(v), n) for v, n in qwords.most_common(16)])
ptr_like = collections.Counter((int.from_bytes(data[i:i + 8], "little") >> 32) for i in range(0, len(data) - 7, 8))
print("common high dwords (pointer regions):", [(hex(v), n) for v, n in ptr_like.most_common(10)])
for row in range(0, min(len(data), 512), 32):
    chunk = data[row:row + 32]
    print(f"{row:05x} {chunk.hex(' ', 8)}")
