#!/usr/bin/env bash
# Dune A/B: runs each build through the same route (title -> desert -> 4 travel rounds, the dune in
# rounds 3-4) and prints frames-per-10 s statistics for the travel phase, side by side.
# Usage: ab_dune.sh OUT_PREFIX BUILD [BUILD...]      e.g. ab_dune.sh X60 78256f4 46e2a7d
# Env: extra KYTY_* variables are passed through; DRIVE_GAME / DRIVE_TEMPLATE as for drive.py.
# Needs >= 10.6 GB available RAM; every run has its own PID-owned watchdog (1024 MiB floor).
cd "$(dirname "$0")/.."
prefix="$1"; shift
avail() { python -c "
import ctypes as c, ctypes.wintypes as w
class M(c.Structure):
    _fields_=[('l',w.DWORD),('ld',w.DWORD),('t',c.c_ulonglong),('a',c.c_ulonglong),('tp',c.c_ulonglong),('ap',c.c_ulonglong),('tv',c.c_ulonglong),('av',c.c_ulonglong),('x',c.c_ulonglong)]
m=M(); m.l=c.sizeof(m); c.windll.kernel32.GlobalMemoryStatusEx(c.byref(m)); print(int(m.a/2**20))"; }
for build in "$@"; do
  out="$prefix-$build"
  free=$(avail)
  if [ "$free" -lt 10600 ]; then echo "$build: only $free MiB available (need 10600), skipped"; continue; fi
  python tools/drive.py launch "builds/$build-clangcl/kyty_emulator.exe" "$out" > /dev/null || continue
  pid=$(python -c "import json;print(json.load(open('tools/drive-state.json'))['pid'])")
  (cd tools && python ram_watchdog.py 1024 "../$out/watchdog.log" > /dev/null 2>&1 &)
  bash tools/route_desert.sh > "$out/route.log" 2>&1
  if ! grep -q "desert reached" "$out/route.log"; then echo "$build: route did not reach the desert"; python tools/drive.py close > /dev/null; continue; fi
  date +%s > "$out/travel_start"
  bash tools/travel.sh 2 > /dev/null
  python tools/stack_sample.py "$pid" Thread_Gpu 300 15 "$out/stacks.txt" > "$out/stack.log" 2>&1 &
  bash tools/travel.sh 2 > /dev/null
  wait
  python tools/drive.py shot end > /dev/null
  python tools/drive.py close > /dev/null
  sleep 8
done
echo "== travel-phase windows (frames per 10 s), last 12 windows of each run"
for build in "$@"; do
  out="$prefix-$build"
  [ -f "$out/frames-emu.csv" ] || continue
  python tools/windows10.py "$out" | tail -13 | head -12 | awk -v b="$build" '{fps[NR]=$4} END {n=asort(fps); printf "%s: min %.1f  p25 %.1f  median %.1f  max %.1f FPS (10 s windows)\n", b, fps[1], fps[int(n/4)+1], fps[int(n/2)+1], fps[n]}'
done
