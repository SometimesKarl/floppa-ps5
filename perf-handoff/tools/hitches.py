"""Frame-time hitches from a KYTY_FRAME_LOG csv: python hitches.py FRAMES.csv [STDOUT_LOG]
Reports the presented-frame interval distribution and, with the emulator log, how much
compile / pipeline time (KYTY_COMPILE_LOG) fell inside each long frame."""
import re, sys, statistics
lines = open(sys.argv[1]).read().splitlines()
freq = int(re.search(r"qpc_frequency=(\d+)", lines[0]).group(1))
hdr = lines[1].split(",")
rows = [dict(zip(hdr, l.split(","))) for l in lines[2:] if l and not l.startswith("#")]
ends = [int(r["present_end_qpc"]) for r in rows if r["kind"] in ("0", "1")]
iv = [(ends[i] - ends[i - 1]) * 1000 / freq for i in range(1, len(ends))]
print(f"frames {len(iv)}  p50 {statistics.median(iv):.1f} ms  p99 {sorted(iv)[int(len(iv)*0.99)]:.1f}  max {max(iv):.0f}")
for t in (50, 100, 250, 500, 1000):
    print(f"  > {t:4d} ms: {sum(x > t for x in iv):4d} frames, {sum(x for x in iv if x > t)/1000:6.1f} s")
if len(sys.argv) > 2:
    ev = []
    for l in open(sys.argv[2], encoding="utf-8", errors="replace"):
        m = re.match(r"(compile|pipeline) qpc=(\d+) (.*)", l)
        if m:
            ms = sum(float(x) for x in re.findall(r"_ms=([0-9.]+)", m.group(3))) if m.group(1) == "compile" \
                else float(re.search(r" ms=([0-9.]+)", m.group(3)).group(1))
            kind = "compile" if m.group(1) == "compile" else re.search(r"kind=(\S+)", m.group(3)).group(1)
            ev.append((int(m.group(2)), kind, ms))
    big = sorted(((iv[i - 1], ends[i - 1], ends[i]) for i in range(1, len(ends)) if iv[i - 1] > 100), reverse=True)[:15]
    for d, a, b in big:
        inside = [e for e in ev if a < e[0] <= b and not e[1].endswith("async")]
        tot = {}
        for _, k, ms in inside:
            tot[k] = tot.get(k, 0) + ms
        print(f"  frame {d:7.0f} ms at +{(a - ends[0]) / freq:6.1f}s: " + ", ".join(f"{k} {v:.0f} ms" for k, v in tot.items()))
