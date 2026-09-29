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

## Cloud session 2026-09-29 additions (not yet built on Windows)

| # | Change | How to verify | Pass |
|---|--------|---------------|------|
| 10 | Fault report prints `red zone protection: enabled/disabled` and 128 bytes below rsp; S20 red-zone hypothesis | Any guest fault shows both lines. S20 test: the DS launch above with `--redzone` appended as an extra emulator argument (`python tools/drive.py launch builds/<sha>-clangcl/kyty_emulator.exe S21-redzone --redzone`, same DRIVE_* variables), played into tutorial gameplay for >= 15 min (S20 crashed at 838 s). Check first that the recorded launch command does not already contain `--redzone` | Report contains both lines. If the BPE job-worker null store (`mov [rax+0x18],r12d`, rax reloaded from `[rsp-0x10]`) stops with `--redzone`, red zone clobbering by host exception dispatch is supported. If it recurs, look for CONTEXT/EXCEPTION_RECORD patterns (0xc0000005, mxcsr 0x1f80) in the below-rsp dump |
| 11 | `CheckBufferRange` in UploadImage / DownloadDepth / DownloadImage | Normal ASTRO desert and DS boot to gameplay | No "range outside the buffer" stop. A stop is a real out-of-range request (or a false positive): send the message, it names guest address, offset, size and buffer size. Rollback: revert the commit |
| 12 | ClearImage aliased-format path requests ColorAttachmentRead\|Write | DS gameplay with KYTY_GPU_STATS | "transit ColorAttachmentOptimal->ColorAttachmentOptimal" count in "rendering ended by" lower than S20 (10/s at 214x120), visuals unchanged |
| 13 | Low-memory guard: a 0 MB reading now counts as low (bug fix, default); opt-in `low_memory_fast_stop=on` / `KYTY_LOW_MEMORY_FAST_STOP=1` stops at once below stop/4 MB or when the last 0.5 s fall would empty RAM within ~1 s | (a) Forced check as for 25aaf07: `KYTY_LOW_MEMORY_STOP_MIB=99999` still stops after 2 s. (b) With fast stop: `KYTY_LOW_MEMORY_STOP_MIB=99999 KYTY_LOW_MEMORY_FAST_STOP=1` stops at the first sample (prints "Low-memory guard: fast stop on" at boot). (c) Normal DS session with fast stop on and the RAM watchdog: note any stop and the free-RAM value in its message | (a),(b) stop with the message; (c) no stop while free RAM stays above 400 MB; a stop during an area load is the intended outcome when RAM would otherwise run out. Rollback: `low_memory_fast_stop=off` |
| 14 | GPU test lane on the real GPU (harness ec6c6f7, PRT tiler 3b4c5a0) | Build `shader_recompiler_compute_tests` (clang-cl) and run `ctest -R "gpu_tiler|shader_recompiler_compute|compute_meta_clear_classification|gpu_command_lane" --output-on-failure` | gpu_tiler passes on AMD. If shader_recompiler_compute / compute_meta_clear_classification also pass on AMD, R7 is lavapipe-only. Any other failure: send the "failed at" line |
| 15 | 2D PRT tiler in game (3b4c5a0) | ASTRO desert + DS gameplay, look at terrain/detail textures | No new texture corruption; if PRT textures looked scrambled before, they now look right |
| 16 | Download staging barrier (df14c37); Vulkan sync validation on a real run | Short ASTRO desert and DS cinematic with the Khronos validation layer installed (Vulkan SDK) and `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (slow: minutes only) | No new rendering problems; collect SYNC-HAZARD lines from stdout and send them (they point at barriers still missing, R1) |
| 17 | texture_ram=trim cooldown: no trim when the same image was uploaded within the last 64 guest frames | DS gameplay with `KYTY_TEXTURE_RAM=trim KYTY_MEMORY_STATS=1`, same route as S20 | "texture RAM trimmed" grows far slower than S20's 17.7 GB in ~10 min; the new "not trimmed (re-uploaded within 64 frames)" figure shows what was skipped; working set stays below ~9 GB (as S20). Fail: WS grows past S20's level, then set the cooldown lower |
| 18 | Reduced quality excludes explicit-LOD samplers (ce63fa2); accounting underflow no longer exits (4a61663) | ASTRO Sky Garden and DS gameplay with `KYTY_TEXTURE_QUALITY=reduced KYTY_MEMORY_STATS=1` (recipe 4) | As recipe 4; additionally no "image accounting underflow" stop, and effects that sample at explicit LOD (reflections, blurred UI) look like full quality |
