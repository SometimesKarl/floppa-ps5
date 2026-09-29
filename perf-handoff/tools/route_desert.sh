#!/usr/bin/env bash
# Drives a drive.py-launched instance from boot to desert gameplay: J at the title, then
# RETURN/DOWN/J once the loading tunnel ends (skips the intro cutscenes), then waits for the desert.
cd "$(dirname "$0")/.."
luma() { echo "$1" | sed -E 's/.*luma=([0-9.]+).*/\1/'; }
in_range() { python -c "import sys; sys.exit(0 if $2<=float('$1')<=$3 else 1)" 2>/dev/null; }
# A screenshot; ends the route when the test process is gone (stopped by the watchdog or closed).
shot() { local out; out=$(python tools/drive.py shot "$1"); case "$out" in *"not running"*|*"window not found"*) echo "route: test process gone" >&2; exit 1;; esac; echo "$out"; }
until python tools/drive.py status 2>/dev/null | grep -q "fps"; do sleep 3; done
n=0
hits=0
while :; do r=$(shot r$n) || exit 1; n=$((n+1)); l=$(luma "$r"); echo "title? $r"
  # The title screen (luma ~44-70) must hold for two polls: logos before it flash through.
  if in_range "$l" 44 70; then hits=$((hits+1)); else hits=0; fi
  if [ $hits -ge 2 ]; then sleep 2; break; fi; sleep 2; done
python tools/drive.py keys J
while :; do r=$(shot r$n) || exit 1; n=$((n+1)); l=$(luma "$r"); echo "tunnel? $r"
  if in_range "$l" 90 200; then break; fi; sleep 2; done
while :; do r=$(shot r$n) || exit 1; n=$((n+1)); l=$(luma "$r"); echo "space? $r"
  if in_range "$l" 0 70; then break; fi; sleep 2; done
sleep 2; python tools/drive.py keys RETURN:0.15:1.0 DOWN:0.15:0.8 J
while :; do r=$(shot r$n) || exit 1; n=$((n+1)); l=$(luma "$r"); echo "desert? $r"
  if in_range "$l" 165 185; then break; fi; sleep 3; done
echo "desert reached"
