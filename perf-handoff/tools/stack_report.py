"""Inclusive/self time per function from stack_sample.py output.

Usage: stack_report.py STACKS.txt EXE [--top N] [--under FUNCTION_SUBSTRING]
Symbolizes EXE-relative addresses with llvm-symbolizer (inlined frames included; return addresses
are looked up one byte back). Inclusive: share of samples with the function anywhere on the stack
(counted once per sample). Self: share of samples where it is the innermost EXE function.
--under restricts to samples whose stack contains a function matching the substring, and reports
shares of those samples.
"""
import collections
import subprocess
import sys
from pathlib import Path

SYMBOLIZER = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
IMAGE_BASE = 0x140000000

args = sys.argv[1:]
top, under = 40, None
if "--top" in args:
    i = args.index("--top")
    top = int(args[i + 1])
    del args[i:i + 2]
if "--under" in args:
    i = args.index("--under")
    under = args[i + 1]
    del args[i:i + 2]
stacks_path, exe = Path(args[0]), Path(args[1])

samples = []
for line in stacks_path.read_text().splitlines():
    n, _, stack = line.partition(" ")
    samples.append((int(n), stack.split(";") if stack else []))

wanted = set()
for _, frames in samples:
    for depth, frame in enumerate(frames):
        if frame.startswith(exe.name + "+0x"):
            offset = int(frame.split("+0x")[1], 16)
            wanted.add(offset - (1 if depth > 0 else 0))
wanted = sorted(wanted)
proc = subprocess.run([SYMBOLIZER, f"--obj={exe}", "--inlining", "--functions=short",
                       "--demangle"], input="\n".join(hex(IMAGE_BASE + o) for o in wanted),
                      capture_output=True, text=True)
names = {}
blocks = proc.stdout.strip().split("\n\n")
for offset, block in zip(wanted, blocks):
    lines = block.splitlines()
    names[offset] = [lines[j] for j in range(0, len(lines), 2)]  # innermost first

total = 0
inclusive, self_counts = collections.Counter(), collections.Counter()
for n, frames in samples:
    functions = []
    for depth, frame in enumerate(frames):
        if frame.startswith(exe.name + "+0x"):
            offset = int(frame.split("+0x")[1], 16) - (1 if depth > 0 else 0)
            functions.extend(names.get(offset, ["?"]))
        else:
            functions.append(frame.split("+0x")[0])
    if under and not any(under in f for f in functions):
        continue
    total += n
    if functions:
        self_counts[functions[0]] += n
    for f in set(functions):
        inclusive[f] += n
if total == 0:
    sys.exit("no samples")
print(f"{total} samples" + (f" under '{under}'" if under else ""))
print("-- inclusive:")
for f, n in inclusive.most_common(top):
    print(f"{100 * n / total:6.1f}%  {f[:150]}")
print("-- self:")
for f, n in self_counts.most_common(min(top, 25)):
    print(f"{100 * n / total:6.1f}%  {f[:150]}")
