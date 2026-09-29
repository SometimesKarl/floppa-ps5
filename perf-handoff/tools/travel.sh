#!/usr/bin/env bash
# Moves Astro around the desert for ROUNDS rounds (~25 s each): run, turn, jump, attack.
# Usage: travel.sh [ROUNDS=3]   (a drive.py instance must be in gameplay)
cd "$(dirname "$0")/.."
rounds="${1:-3}"
for r in $(seq 1 "$rounds"); do
  python tools/drive.py keys W:3.0:0.2 D:0.8:0.2 W:3.0:0.2 J:0.2:0.4 W:2.0:0.2 K:0.2:0.6 \
    A:1.5:0.2 W:3.0:0.2 J:0.2:0.3 J:0.2:0.6 S:2.0:0.2 K:0.2:0.6 D:1.2:0.2 W:2.5:0.3 > /dev/null
  echo "round $r done"
done
