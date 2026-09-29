"""Aggregate IP samples (thread_sample.py dump) by function, symbolized against a PDB.

Usage: symbolize_samples.py DUMP EXE [TOP=40]
DUMP lines: "<count> <module>+0x<offset>" (or anon@...). Offsets inside EXE's module name are
symbolized with llvm-symbolizer (VS BuildTools LLVM); inlined frames are attributed to both the
innermost function ("self") and every enclosing inlined function ("incl").
"""
import collections
import subprocess
import sys
from pathlib import Path

SYMBOLIZER = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
dump = Path(sys.argv[1])
exe = Path(sys.argv[2])
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
IMAGE_BASE = 0x140000000

counts = collections.Counter()
other = collections.Counter()
for line in dump.read_text().splitlines():
    n, loc = line.split(" ", 1)
    if loc.startswith(exe.name + "+0x"):
        counts[int(loc.split("+0x")[1], 16)] += int(n)
    else:
        other[loc.split("+")[0].split("@")[0]] += int(n)
total = sum(counts.values()) + sum(other.values())
offsets = sorted(counts)
text = subprocess.run([SYMBOLIZER, f"--obj={exe.resolve()}", "--inlining"],
                      input="\n".join(hex(IMAGE_BASE + o) for o in offsets) + "\n",
                      capture_output=True, text=True).stdout
# Output: per address, pairs of lines (function, file:line), frames innermost first, blank line.
blocks = text.strip("\n").split("\n\n")
self_t = collections.Counter()
incl_t = collections.Counter()
for off, block in zip(offsets, blocks):
    lines = [l for l in block.splitlines() if l.strip()]
    funcs = lines[0::2]
    if not funcs:
        continue
    self_t[funcs[0]] += counts[off]
    for f in set(funcs):
        incl_t[f] += counts[off]
print(f"{total} samples; outside {exe.name}: " + ", ".join(f"{k} {100 * v / total:.1f}%" for k, v in other.most_common(6)))
print("self%   incl%   function")
for f, n in self_t.most_common(top):
    print(f"{100 * n / total:5.1f}  {100 * incl_t[f] / total:6.1f}   {f[:110]}")
