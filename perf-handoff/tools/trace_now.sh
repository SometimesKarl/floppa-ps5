#!/usr/bin/env bash
# Captures a Tracy trace from the running emulator (started with --profile) for N seconds
# into OUT_DIR and exports the zone tables used by zone_table.py / gpu_timeline.py.
# Usage: trace_now.sh OUT_DIR [SECONDS=20]
T="$(dirname "$0")/tracy-0.14.1"
out="$1"; secs="${2:-20}"
mkdir -p "$out"
"$T/tracy-capture.exe" -o "$out/trace.tracy" -a 127.0.0.1 -p 8086 -f -s "$secs" > "$out/tracy-capture.log" 2>&1
"$T/tracy-csvexport.exe" "$out/trace.tracy" > "$out/tracy-zones.csv"
"$T/tracy-csvexport.exe" -e "$out/trace.tracy" > "$out/tracy-zones-self.csv"
"$T/tracy-csvexport.exe" -g "$out/trace.tracy" > "$out/tracy-gpu.csv"
ls -la "$out" | tail -4
