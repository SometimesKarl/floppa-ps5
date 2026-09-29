"""Aggregate IP samples (thread_sample.py dump) by source line, for chosen functions.

Usage: sample_lines.py DUMP EXE [FUNCTION_SUBSTRING ...] [--top N]
Symbolizes every sampled offset in EXE with llvm-symbolizer (line tables needed: release builds
carry -gline-tables-only) and prints the hottest innermost file:line entries, optionally only for
samples whose inlined call chain contains one of the substrings.
"""
import collections
import subprocess
import sys
from pathlib import Path

SYMBOLIZER = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
IMAGE_BASE = 0x140000000

args = sys.argv[1:]
top = 40
if "--top" in args:
    i = args.index("--top")
    top = int(args[i + 1])
    del args[i:i + 2]
dump, exe, filters = Path(args[0]), Path(args[1]), args[2:]

counts = collections.Counter()
total = 0
for line in dump.read_text().splitlines():
    n, loc = line.split(" ", 1)
    total += int(n)
    if loc.startswith(exe.name + "+0x"):
        counts[int(loc.split("+0x")[1], 16)] += int(n)
offsets = sorted(counts)
out = subprocess.run([SYMBOLIZER, f"--obj={exe.resolve()}", "--inlining", "--relative-address"],
                     input="\n".join(hex(o) for o in offsets) + "\n", capture_output=True,
                     text=True).stdout
blocks = out.strip().split("\n\n")
by_line = collections.Counter()
by_func = collections.Counter()
picked = 0
for offset, block in zip(offsets, blocks):
    frames = block.strip().splitlines()
    pairs = [(frames[i], frames[i + 1] if i + 1 < len(frames) else "?") for i in range(0, len(frames), 2)]
    chain = " ".join(f for f, _ in pairs)
    if filters and not any(f in chain for f in filters):
        continue
    n = counts[offset]
    picked += n
    func, where = pairs[0]
    where = where.replace("C:\\Users\\himav\\Desktop\\kyty ps5-src\\src\\", "")
    by_line[f"{where}  [{func[:70]}]"] += n
    by_func[func[:110]] += n
print(f"{total} samples; {picked} matched ({100.0 * picked / max(total, 1):.1f}%)")
print("-- hottest lines (innermost frame):")
for key, n in by_line.most_common(top):
    print(f"{100.0 * n / total:5.2f}%  {key}")
print("-- hottest innermost functions among matched:")
for key, n in by_func.most_common(15):
    print(f"{100.0 * n / total:5.2f}%  {key}")
