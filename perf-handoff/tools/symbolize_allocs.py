"""Symbolizes the last KYTY_ALLOC_SAMPLER dump: symbolize_allocs.py STDOUT_LOG EXE [TOP=25]"""
import re
import subprocess
import sys
from pathlib import Path

SYM = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
exe = Path(sys.argv[2]).resolve()
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
start = max(i for i, l in enumerate(lines) if l.startswith("alloc sampler:"))
print(lines[start])
rows = []
for l in lines[start + 1:]:
    if not l.startswith("alloc ") or l.startswith("alloc sampler"):
        break
    rows.append(l.split())
rows = rows[:top]
offs = sorted({int(x, 16) for r in rows for x in r[3:] if int(x, 16)})
names = {}
for o in offs:
    out = subprocess.run([SYM, f"--obj={exe}", hex(0x140000000 + o)],
                         capture_output=True, text=True).stdout.splitlines()
    names[o] = out[0] if out else "?"


def short(n):
    n = re.sub(r"\(.*", "", n)
    return n[-80:]


for r in rows:
    frames = [short(names.get(int(x, 16), "?")) for x in r[3:] if int(x, 16)]
    frames = [f for f in frames if not re.search(r"^std::|operator new|_Allocate|allocator", f)][:3]
    print(f"{int(r[1]):9d} allocs {int(r[2]) / 1e6:8.1f} MB  " + " <- ".join(frames))
