"""Lists the SPIR-V modules of a KytyPS5 <title>.pipelines file (PipelinePrewarm format) and writes the
largest ones out as .spv files.

Usage: pipelines_modules.py FILE.pipelines [OUT_DIR] [TOP=10]
"""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1])
out = Path(sys.argv[2]) if len(sys.argv) > 2 else None
top = int(sys.argv[3]) if len(sys.argv) > 3 else 10
data = path.read_bytes()
magic, version = struct.unpack_from("<II", data, 0)
assert magic == 0x5750594B, "not a KYPW file"
pos, modules, counts = 8, {}, {1: 0, 2: 0, 3: 0}
while pos + 5 <= len(data):
    kind, size = struct.unpack_from("<BI", data, pos)
    pos += 5
    payload = data[pos:pos + size]
    pos += size
    counts[kind] = counts.get(kind, 0) + 1
    if kind == 1:
        (h,) = struct.unpack_from("<Q", payload, 0)
        modules[h] = payload[8:]
sizes = sorted(((len(w) // 4, h) for h, w in modules.items()), reverse=True)
total = sum(s for s, _ in sizes)
print(f"{len(modules)} modules, {total} words total; graphics records {counts.get(2, 0)}, compute {counts.get(3, 0)}")
for q in (0.5, 0.9, 0.99):
    print(f"  p{int(q * 100)} {sizes[int((1 - q) * len(sizes))][0]} words")
print("  largest:", [s for s, _ in sizes[:top]])
if out:
    out.mkdir(parents=True, exist_ok=True)
    for i, (s, h) in enumerate(sizes[:top]):
        (out / f"{i:02d}_{h:016x}_{s}.spv").write_bytes(modules[h])
