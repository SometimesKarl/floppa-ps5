# Local test recipe (owner's PC)

Every entry: what to build, how to run, what to read, pass/fail. Always keep the RAM watchdog running
(`tools/ram_watchdog.py 1024 <run>/watchdog.log`) and close Discord/browsers first (tests need ~10.6 GB
free for ASTRO, and Demon's Souls gameplay needs `KYTY_TEXTURE_RAM=trim` on 16 GB).

Common launch (from `Performance Experiments`):
- ASTRO BOT: `python tools/drive.py launch builds/<sha>-clangcl/kyty_emulator.exe X<nn>-<name>` then
  `bash tools/route_desert.sh` and `bash tools/travel.sh 4` (dune in rounds 3-4), or
  `bash tools/ab_dune.sh X<nn> 78256f4 <sha>` for an A/B. The game path is taken from
  `Performance Handoff/system-and-launch.json`; set `DRIVE_GAME=<path to eboot.bin>` if the game moved.
- Demon's Souls: `DRIVE_TEMPLATE=$PWD/testbed/ds-fresh DRIVE_GAME='S:/[SuperPSX]-Demons.Souls-PPSA01342-USA-PS5/[SuperPSX]-Demons.Souls-PPSA01342-USA-PS5/PPSA01342-app0/eboot.bin' DRIVE_PATCH=none DRIVE_NO_VALIDATION=1 KYTY_TEXTURE_RAM=trim python tools/drive.py launch ...`;
  it reaches tutorial gameplay by itself after ~9-10 minutes (first run compiles many shaders).
- Stats: `KYTY_HITCH_LOG=1 KYTY_GPU_STATS=1 KYTY_MEMORY_STATS=1` (+ `KYTY_UPLOAD_LOG=1` for transfers).

## Pending verification from the 2026-09-28/29 session (build 9c52585 contains all of these)

| # | Change | How to verify | Pass |
|---|---|---|---|
| 1 | 9c52585 depth target sampled read-only | DS gameplay with KYTY_GPU_STATS | "transit DepthReadOnlyStencilAttachment..." gone from "rendering ended by"; begin rendering well below S20's 831/s; visuals unchanged (fog, lighting, HUD) |
| 2 | d3ce7f8 / 8df78aa / a424fbb walk speedups | `ab_dune.sh X60 78256f4 9c52585`; separately `KYTY_SRT_VERIFY=1 KYTY_CLEAN_READ_VERIFY=1` on a desert run | dune 10-s windows higher than 78256f4; 0 mismatches |
| 3 | 0521196 emergency eviction keeps recent images | DS gameplay, KYTY_MEMORY_STATS | "emergency-freed" and "written back" far below S12's 3,006 / 1.9 GB per 30 s |
| 4 | 46e2a7d texture_quality=reduced | ASTRO Sky Garden and DS gameplay with `KYTY_TEXTURE_QUALITY=reduced` | device usage below budget, "reduced quality: N created", no visual corruption; FPS vs full |
| 5 | df00497 texture_ram=trim | DS gameplay | RAM stays below ~9 GB WS; "texture RAM trimmed" grows slowly (not tens of GB) |
| 6 | 25aaf07 low-memory guard | normal play | title shows "LOW RAM" warning before trouble; no whole-PC freezes |
| 7 | 0629bef APR single query | DS boot time to the cinematic vs S12 | faster, same behaviour |
| 8 | e49dd65 fault handler | any crash: the report ends with one "Unhandled host exception" (or one "nested host exception" line), no repeated fault contexts | single clean report |
| 9 | Upstream merges (8560b11 etc.) | ASTRO desert + dune, DS cinematic | no new errors, visuals correct |
