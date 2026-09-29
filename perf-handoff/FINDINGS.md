# KytyPS5 performance work: findings (ASTRO BOT + Demon's Souls), as of 2026-09-29

Everything below was measured on the owner's PC unless marked **unverified** or **hypothesis**.
Raw logs stay on that PC; curated measurement lines are in `logs/`, planning history in `docs/`.

## 1. Test machine and games

| | |
|---|---|
| CPU | AMD Ryzen 5 5500 (Zen 3, 6 cores / 12 threads) |
| GPU | AMD Radeon RX 6650 XT, 8 GB (Adrenalin 32.0.21045.5002, Vulkan driver 2.0.353); VMA device budget ~6.9-7.4 GB |
| RAM | 16 GB (16,277 MB); Windows + tools use 4-6 GB; commit limit ~65 GB (pagefile on the NVMe) |
| Disks | C: NVMe 512 GB (~100 GB free), S: 1 TB 5400 rpm HDD (WD10SPZX), D: 500 GB HDD |
| Display | 190 Hz; tests run windowed 1280x720 |
| ASTRO BOT | PPSA21564 v01.007.000 (+ memory-restore game patch). Game files were being moved from C: to S: at session end |
| Demon's Souls | PPSA01342 v01.004.000, no patch, 164 GB in 223,264 loose files on S: (HDD) |

Working branch: `local/merge-upstream-0929` (upstream KytyPS5 main merged to 59a1760 plus 127 own commits,
list in `docs/OUR-COMMITS.txt`). HEAD 9c52585 builds (Windows clang-cl, `build/build-clangcl-release.cmd`).
The owner's day-to-day build (`Optimized Build`) is still **78256f4**.

## 2. Where the time goes

### ASTRO BOT
| Scene | Result | Source |
|---|---|---|
| Desert, legacy DRS steering to ~1080p | ~37 FPS, p50 31.5 ms, p90 34 ms, p99 64 ms (native 4K: 22.6 FPS) | docs/MASTER-PLAN.md section 2 |
| Dune (sand trails, ~14k tiny draws/s) | 14 FPS before af83bbf, 18-22 FPS 10-s windows after (X43, 78256f4) | logs/X43-dune-78256f4.txt |
| Dune frame time | 50-70 ms, all "other" (GPU-thread CPU), gpu-wait ~0 | logs/X52-perf-adb9a0c-incr0.txt |
| Sky Garden | ~3-4 FPS: textures 4.75 GB overflow VRAM, device-local buffers spill to RAM, RAM at 173 MB free -> PC froze (9/28) | docs/WORKLOG.md 9/28 02:00 |

Dune GPU-thread profile (X52, 300 samples, `logs/X52-*-profile*.txt`):
MaterializeResources (SRT walk) 34% [FindActiveSources 16%, RefreshFlatBuffer 11%, strict guest reads ~7%];
ExecutePreparedDraw 27% [amdvlk recording ~6%, BufferCache::ObtainBuffer/uploads ~6%, texture lookups ~3%];
the rest spread thin (pipeline lookup, bindings, per-draw bookkeeping). No single item above 35%.

### Demon's Souls
| Run (build) | Result |
|---|---|
| S7 (bb021fa, old) | character creation 1.7 FPS |
| S11 (adb9a0c) | fresh save folder boots; opening cinematic ~31 FPS; character creation 2-19 FPS |
| S12 (a4d09c0) | tutorial gameplay ~3.5 FPS: frame ~290 ms = other 200 + gpu-wait 60 + buffer 20; 18.4k indirect draws/s (~5k/frame); 2560x1440 color binds 23.8k/s; VRAM 6,984/6,984 MiB; 3,006 emergency evictions and 1.9 GB written back per 30 s; 1,553 render passes/s, 1,239/s ended by Image::Transit |
| S13-S16 | image uploads 52-57 GB per 10 s; the 3840x2160 RGBA16F target deleted/re-created 248x per 10 s by ResolveOverlap (tick-based "safe to delete"); partial uploads skipped ~0.5 GB |
| S17 (0521196) | **whole-PC freeze** ~4 min into loading (Kernel-Power 41, BugcheckCode 0, no TDR event); RAM 4.7 GB free, VRAM 4.5 GB free. First run with multi-band partial uploads active. Cause NOT isolated |
| S18 (fc8a947) | boot faster (frame 1837 at 31 s vs 1788 at 104 s in S12; not isolated); cinematic 24-27 FPS; **churn gone**; partial uploads skip 12.5-14.2 GB per 10 s (real traffic ~30 instead of ~55 GB); stopped by the RAM watchdog at 757 s (WS 9.6 GB, free RAM 28 MB; the owner's file copy held ~8 GB of cache) |
| S19 (f701283, texture_ram=trim) | WS jumped 6.8 -> 10.6 GB in ~11 s at ~545 s (private bytes 8.6 -> 8.9 GB: guest pages made resident in a burst); trim released 131 GB (churned on compute-rewritten textures) |
| S20 (df00497, trim fixed) | reached gameplay, correct visuals (HUD, fog, character); WS 8.7-9.0 GB with 2.9 GB free at 834 s (S18/S19 ran out earlier); gameplay 1.9-2.4 FPS during first-visit shader compiles (pipeline hitches 177 ms, other ~270 ms, gpu-wait ~90-99 ms); 13.6k draws/s (12.3k indirect); 831 passes/s of which **673/s "transit DepthReadOnlyStencilAttachment -> same, 2560x1440 D32S8"**; buffer uploads 137 MiB per 10 s in 4,250 uploads; trim total 17.7 GB (still re-trims); **crashed at 838 s** (section 4) |

Demon's Souls CPU side (S12 thread samples): 13 guest "BPE JobWorkerThread" threads; 3 at ~95% of a core
spinning in a work-stealing scan (guest code 0x90086e000-0x90086ffff: 256 slots then 15 Chase-Lev
deques, `lock cmpxchg`), 8 at ~50%; one worker spent 93% of samples in
`Apr::ResolveFilepathsToIdsAndFileSizes -> ResolveOnePath -> IsDirectoryExisting` (NtQueryAttributesFile,
first-time lookups of loose files on the HDD). Indexed indirect draws never use the CPU-argument
fallback (0 "CPU reads the arguments" messages in S12/S15/S17).

### RAM composition
ASTRO boot (75 s): WS 5.7 GB = guest-mapped 4.34 GB + host-private 0.95 GB (X56). DS cinematic: WS 4.7 GB =
guest 3.5 GB + host-private 1.07 GB (S18). The emulator mirrors guest (PS5) memory 1:1 in PC RAM, and GPU
resources live in both guest RAM and VRAM. Flexible-memory zero-fill (6bf26df) changed nothing at boot.
RAM bursts of 2-4 GB in ~10 s happen during DS area loads; CPU buffer uploads are only MBs per 10 s
(S20), so the burst is guest memory being touched (game streaming and/or emulator first reads of
GPU-only memory: **hypothesis, unmeasured**).

## 3. Root causes established

1. **Per-draw CPU cost on the single GPU command thread** (~50-70 us per draw). Dominant for the dune and
   Demon's Souls gameplay. Spread over resource walks, binding preparation, buffer/texture cache
   lookups and driver recording.
2. **Render-pass breaks.** Dune: 17k/s from read-only depth and read-after-read barriers (fixed af83bbf).
   DS gameplay: 673/s from the depth target being both sampled and bound with two different access masks
   (fix 9c52585, unverified); ~280/s from guest release/flush global barriers (graphicsRun.cpp
   EmitGlobalBarrier); buffer uploads end passes (bufferCache SynchronizeBuffer, streamBuffer).
3. **Resource churn / transfers.** DS render targets re-created by overlap resolution (fixed 5fd3a15);
   compute shaders write images through buffers every frame, each forcing an upload (detile to a scratch
   buffer, then copyBufferToImage). Upload bandwidth halved by persisting targets; the remaining traffic
   is necessary in the current design.
4. **VRAM exhaustion** (DS gameplay, ASTRO Sky Garden) -> emergency eviction of images in use (1 s idle
   rule at 3.5 FPS = 3 frames) -> re-create + re-upload thrash. Policy fixed (0521196, unverified in
   gameplay); opt-in reduced texture quality (46e2a7d, never run).
5. **RAM on 16 GB**: guest mirror + VRAM duplicates + Windows. Freezes happen when free RAM reaches ~0
   (9/28). Guard added (25aaf07, tested); opt-in texture RAM trimming lets DS gameplay fit (S20).
6. **Crashes**: S17 machine freeze (unknown cause; multi-band upload path and upstream #894 are
   suspects; bounds checks added df9fec0); S20 guest fault + fault-handler recursion (section 4).
7. **Loading on the HDD**: APR path lookups did 3 file-system queries per new path (0629bef: one).

## 4. The S20 crash (logs/S20-crash-report.txt)

A guest fault in `BPE JobWorkerThread CPU1` at guest pc 0x900b3c079 (rax=0, rbx=0, rbp=1, rcx=-1, code
around `f3 48 0f bd` = lzcnt/bsr loops, `c4 e2 e0 f3` = BMI blsr): the emulator's
`Loader::KytyExceptionHandler` then called `PrintHostBacktrace` (runtimeLinker.cpp:673), which faulted
inside ntdll (pc ntdll+0x1587a, access address 0x1) and re-entered the handler 5 times before the
process ended ("Unhandled host exception ... runtimeLinker.cpp:768"). Two problems: (a) why the guest
faulted (an HLE result or memory state the guest did not expect; unknown), (b) the fault handler is not
re-entrancy safe while printing a host backtrace.

## 5. Change ledger (this effort; full list docs/OUR-COMMITS.txt)

Status: **M** measured on the PC, **V** verified correct on the PC (no perf number), **U** built,
unverified, **R** reverted, **O** opt-in (off by default).

| Commit | Change | Status |
|---|---|---|
| af83bbf | read-only depth attachments, read-after-read barrier skip | M: dune passes 17k -> 1.5k/s, 14 -> ~20 FPS (deployed) |
| ce95f9f | partial render-target uploads (row bands, RT 64 KiB tiling) | M: ~57% of RT upload bytes skipped (deployed) |
| 6ea9551 | window-title notice while compiling shaders | V (deployed) |
| 78256f4 | RAM floor for GPU spills to system memory | deployed |
| d6ca050 / 360189d | SRT memo bypass / memo validation order | M: small (walk 40 -> 36%) |
| adb9a0c -> a4d09c0 | incremental SRT walks | R: correct but slower |
| 0629bef | APR: one file-system query per new path | U (boot faster in S18, not isolated) |
| c837382 / 3e6ecc8 | standard-64K band uploads / dirty 64ths | O since df9fec0 (KYTY_PARTIAL_UPLOADS_EXT=1); suspect in S17 freeze |
| d54c0f9 | one mapping lookup per guest backing transfer | U |
| ef996b5 | KYTY_IMAGE_CHURN_LOG; ImageResource::exact_texels | V (found the DS churn) |
| 5fd3a15 | overlap resolution keeps images used in the last 2 guest frames | M: DS churn gone, partial skips 12.5-14 GB/10 s |
| 17363db, ce71faa, 8560b11 | upstream merges (36 commits) | V: ASTRO boots, 458,752 walks 0 mismatches |
| d3ce7f8 | FindActiveSources per-plan base + decoded condition roots | V (KYTY_SRT_VERIFY 0 mismatches), perf U |
| 8df78aa | per-walk cache of GPU-clean 4 KiB pages for strict reads | V (9.37 M cached reads 0 mismatches), perf U |
| a424fbb | observed walker reuses clean-walker values (KYTY_SRT_SHARE_CLEAN=0 off) | V, perf U |
| 0521196 | emergency eviction keeps images used in the last 8 frames, read-only first | U |
| df9fec0 | bounds checks on image copies; extended bands opt-in | V (no false positives in S18-S20) |
| 6bf26df | flexible memory: zero only reused backing | M: no change at boot |
| 25aaf07 | low-memory guard (title warning < 1 GB, clean stop < 400 MB for 2 s) | M: forced trigger stops in 2 s |
| 46e2a7d | texture_quality=reduced (drop top mip of sampled-only textures) | O, never run |
| fc8a947 | GPU stats: render-pass breaks attributed to image transitions | V (found the DS depth issue) |
| f701283 / df00497 | texture_ram=trim (VirtualUnlock uploaded CPU-written textures) | O, M: DS gameplay fits in RAM (S20) |
| 9c52585 | depth target sampled read-only: one access for both transitions | U (targets 673 breaks/s) |

Rejected with data: graphics pipeline library fast link (link ~= full compile on this driver),
spirv-opt (-10% compile for +90% CPU), branchless guarded loads (-9% compile but invalidates driver
caches), VS/PS walk parallelism (<= 10%), incremental SRT walks (slower), native-1080p "output mode"
report alone (ASTRO keeps two 3840x2160 display buffers; see docs/NATIVE-1080P-PLAN.md).

## 6. Tooling (tools/)
drive.py (owned-process launch, keys, screenshots; DRIVE_GAME / DRIVE_PATCH / DRIVE_TEMPLATE /
DRIVE_NO_VALIDATION), ram_watchdog.py (PID-owned, logs test WS at low RAM), route_desert.sh + travel.sh
(ASTRO route), ab_dune.sh (A/B), windows10.py (FPS per 10 s), stack_sample.py + stack_report.py
(dbghelp stack sampling, llvm-symbolizer), thread_top.py / thread_sample.py (THREAD_SAMPLE_EXACT=1),
guest_disasm.py (disassemble guest code of the test process), resident_breakdown.py / ws_growth.py (RAM),
pipebench (driver compile timing). Emulator diagnostics: KYTY_HITCH_LOG, KYTY_GPU_STATS,
KYTY_MEMORY_STATS, KYTY_UPLOAD_LOG, KYTY_IMAGE_CHURN_LOG, KYTY_SRT_MEMO_STATS, KYTY_SRT_VERIFY,
KYTY_CLEAN_READ_VERIFY, KYTY_FRAME_LOG.
