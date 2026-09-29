"""Counts samples by the source line of a caller function, for samples containing a target.

Usage: stack_site.py STACKS.txt EXE TARGET_SUBSTRING CALLER_SUBSTRING
For each sample whose stack (inlined frames included) contains TARGET_SUBSTRING, finds the frame
of CALLER_SUBSTRING and counts its file:line: e.g. which call in GetGraphicsPrograms (vertex or
pixel program) the MaterializeResources samples came from.
"""
import collections
import subprocess
import sys

SYMBOLIZER = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\llvm-symbolizer.exe"
IMAGE_BASE = 0x140000000

stacks, exe, target, caller = sys.argv[1:5]
exe_name = exe.replace("\\", "/").split("/")[-1]
samples = []
for line in open(stacks):
    count, _, stack = line.strip().partition(" ")
    samples.append((int(count), stack.split(";") if stack else []))


def offset(frame, index):
    return int(frame.split("+0x")[1], 16) - (1 if index else 0)


wanted = sorted({offset(f, i) for _, frames in samples for i, f in enumerate(frames)
                 if f.startswith(exe_name)})
out = subprocess.run([SYMBOLIZER, f"--obj={exe}", "--inlining", "--functions=short", "--demangle"],
                     input="\n".join(hex(IMAGE_BASE + o) for o in wanted), capture_output=True,
                     text=True).stdout.strip().split("\n\n")
chains = {}
for o, block in zip(wanted, out):
    lines = block.splitlines()
    chains[o] = [(lines[j], lines[j + 1].replace("\\", "/").split("/")[-1])
                 for j in range(0, len(lines) - 1, 2)]

sites = collections.Counter()
total = 0
for count, frames in samples:
    names = []
    for i, frame in enumerate(frames):
        if frame.startswith(exe_name):
            names += chains.get(offset(frame, i), [])
    if not any(target in name for name, _ in names):
        continue
    total += count
    # The frame just inside the caller: the caller's own entry carries the call site line.
    site = next((where for name, where in names if caller in name), "no caller")
    sites[site] += count
print(f"{total} samples containing '{target}'")
for site, count in sites.most_common():
    print(f"{count:5d}  {site}")
