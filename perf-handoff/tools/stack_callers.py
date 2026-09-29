"""Where a function is called from, in stack_sample.py output.

Usage: stack_callers.py STACKS.txt EXE FUNCTION_SUBSTRING [DEPTH=6]
For samples whose innermost EXE frames (inlined frames included) contain FUNCTION_SUBSTRING within
the first three names, prints the most common chains of the innermost DEPTH names with file:line.
"""
import collections
import subprocess
import sys

SYMBOLIZER = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
IMAGE_BASE = 0x140000000

stacks, exe, needle = sys.argv[1], sys.argv[2], sys.argv[3]
depth = int(sys.argv[4]) if len(sys.argv) > 4 else 6
exe_name = exe.replace("\\", "/").split("/")[-1]

samples = []
for line in open(stacks):
    count, _, stack = line.strip().partition(" ")
    samples.append((int(count), stack.split(";") if stack else []))


def offset(frame, index):
    return int(frame.split("+0x")[1], 16) - (1 if index else 0)


wanted = sorted({offset(f, i) for _, frames in samples for i, f in enumerate(frames[:8])
                 if f.startswith(exe_name)})
out = subprocess.run([SYMBOLIZER, f"--obj={exe}", "--inlining", "--functions=short", "--demangle"],
                     input="\n".join(hex(IMAGE_BASE + o) for o in wanted), capture_output=True,
                     text=True).stdout.strip().split("\n\n")
chains = {}
for o, block in zip(wanted, out):
    lines = block.splitlines()
    chains[o] = [(lines[j], lines[j + 1].replace("\\", "/").split("/")[-1])
                 for j in range(0, len(lines) - 1, 2)]

counts = collections.Counter()
total = 0
for count, frames in samples:
    names = []
    for i, frame in enumerate(frames[:8]):
        if frame.startswith(exe_name):
            names += chains.get(offset(frame, i), [])
    if any(needle in name for name, _ in names[:3]):
        total += count
        counts[" < ".join(f"{name[:45]}@{where}" for name, where in names[:depth])] += count
print(f"{total} samples with '{needle}' innermost")
for chain, count in counts.most_common(12):
    print(count, chain)
