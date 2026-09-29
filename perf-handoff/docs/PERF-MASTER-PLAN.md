# Performance master plan: dune, CPU, RAM, VRAM (ASTRO BOT + Demon's Souls)

Written 2026-09-29. Governs the performance work under OPUS-AUTONOMOUS-MASTER-PLAN.md (safety, test and
deployment rules unchanged). User priorities: the ASTRO BOT dune first, Demon's Souls as the heavy stress test,
fixes that serve both games. NATIVE-1080P-PLAN.md stays the resolution research track.

## 1. Where the time goes (measured)

| Scene | Frame | Limit | Evidence |
|---|---|---|---|
| ASTRO desert (travel) | 25-30 ms | mixed | X51/X52 windows 30-37 FPS |
| ASTRO dune (sand trails) | 50-70 ms (14-22 FPS) | **GPU-thread CPU only** ("other" 46-66 ms, no gpu-wait) | X52 hitch log; ~14k draws/s of <= 6 vertices x instances |
| DS cinematic | 30-40 ms | mixed | S11/S13 ~26-31 FPS |
| DS tutorial gameplay | ~290 ms (~3.5 FPS) | GPU-thread CPU 200 ms + gpu-wait 60 ms + buffer 20 ms; **VRAM full** (6,984/6,984 MiB, 3,006 emergency evictions and 1.9 GB written back per 30 s) | S12 |

GPU-thread profile on the dune (X52, 300 samples):
MaterializeResources (SRT walk) 34% [FindActiveSources 16%, RefreshFlatBuffer 11%, strict reads ~7%];
ExecutePreparedDraw 27% [amdvlk recording ~6%, ObtainBuffer/uploads ~6%, texture lookups ~3%];
PrepareGraphicsBindings, pipeline lookup, per-draw bookkeeping the rest. No single item above 35%.

## 2. Done this session (with status)

| Change | Status |
|---|---|
| af83bbf read-only depth + read-after-read barrier skip | measured: dune render passes 17k/s -> ~1.5k/s, 14 -> ~20 FPS; deployed (78256f4) |
| ce95f9f partial render-target uploads | measured: ~57% of RT upload bytes skipped (ASTRO); deployed |
| adb9a0c incremental SRT walks | correct (3.08 M walks, 0 mismatches) but slower (walk 45% vs 34%): reverted |
| 0629bef APR path lookup: 1 file-system query instead of 3 | built, DS load-time effect unmeasured |
| 3e6ecc8 dirty bytes per 64th of an image | built; DS: no change (writes really cover the images) |
| d54c0f9 one mapping lookup per guest backing transfer | built, unmeasured |
| 5fd3a15 overlap resolution keeps images used in the last 2 frames | fixes DS 4K target delete/re-create 25x/s (S16 churn log); runtime check pending |
| 17363db merge of upstream main (539c0f7, 23 commits incl. per-plan resource policy, CPU-ownership dirty regions, extended memory) | builds; runtime check pending |
| d3ce7f8 FindActiveSources: per-plan base mask + decoded condition roots | built; verify with KYTY_SRT_VERIFY=1, then dune A/B |

Review of the NATIVE-1080P research branch (codex/native-1080p-research, 4 commits): diagnostics only.
It proved that ASTRO registers two 3840x2160 display buffers whatever output mode is reported, and that the
scanout pair is only ~64 MB. Its one behaviour change (legacy DRS steering off by default) would remove the
measured desert gain (23 -> 37 FPS), so it is not merged. Open risk it raised: the steering scales a
guest-visible clock (up to 8x): watch for timing/animation side effects.

## 3. Work packages (adopted 2026-09-29 from the user's review; three tracks + resolution research)

Track A = resource ownership / transfers, Track B = draw preparation, Track C = shader + loading
reliability. One game test at a time. Accept changes on action-route frame times, foreground compile
stalls, transfer volume and memory peaks, not on promised FPS.

| # | Package | Track | Evidence / status |
|---|---|---|---|
| 1 | Keep GPU data on the GPU, fix resource ownership | A | DS: 4K target deleted/re-created 25x/s (fixed 5fd3a15, unmeasured); DS/ASTRO: compute writes through buffers re-upload whole images each frame (up to 55 GB / 10 s): needs "newest copy" tracking, not bigger bands. Extended band uploads off after the freeze (df9fec0). |
| 2 | Compiled resource walks (decode once, execute compactly, reuse scratch) | B | Walk ~34% of the dune GPU thread. Incremental cache was slower (reverted). Done so far: per-plan FindActiveSources base + condition roots (d3ce7f8), clean-page read cache (8df78aa), clean/observed value reuse (a424fbb): verified, unmeasured. Next: flat per-plan op list. |
| 3 | Less repeated descriptor / draw-state preparation | B | ExecutePreparedDraw ~27% (ObtainBuffer, texture lookups, driver recording). Upstream 59a1760 merge added some of this; measure first. |
| 4 | GPU-generated indirect work; batch compatible draws | B | DS: ~18k indirect draws/s. First check whether any indirect args are read back or split on the CPU; count eligible consecutive compatible draws before building batching. |
| 5 | Fewer sync points and render-pass breaks | A/B | Dune fixed earlier (17k -> 1.5k passes/s, af83bbf). DS: 1,239 of 1,553 passes/s end in Image::Transit; fc8a947 attributes them by transition/image. |
| 6 | Dependable shader reuse; then cheaper generated shaders | C | Prewarm + driver cache work on revisits; first-encounter compiles remain. GPL / spirv-opt rejected with data; revisit only with a new hypothesis. |
| 7 | Guest scheduling / completion delays | B | DS: 13 job workers busy-scanning; upstream 6f24b03 (short sleeps block) merged; measure CPU and completion latency before changing anything. |
| 8 | Bounded loading and residency | C | 0629bef (1 file query per APR path); low-memory guard (25aaf07, tested); emergency eviction keeps recently used images (0521196); opt-in texture_quality=reduced (46e2a7d). Game files on the HDD slow DS (and ASTRO if moved there). |
| 9 | Real scene/output resolution + expensive GPU passes | research | NATIVE-1080P-PLAN.md: display buffers 2x 64 MiB only; scene/depth/history/post resources still unmapped. Legacy DRS steering kept (desert 23 -> 37 FPS). |

Threading expectation (corrected): parallelising only the walk (~34%) on 4 workers gives at most
1 / (0.66 + 0.34/4) = 1.34x on the dune, removing it entirely 1.52x; larger gains need packages 1-5
too. Parallel preparation is built on top of 2 and 3, with a two-worker prototype first.

Cheap wins alongside: no hot-path allocations, immutable decoding cached, scratch reused, duplicate
file-system queries removed, background workers bounded.

## 4. Rules for every package
Commit before building; stage under builds\<sha>-clangcl; owned-PID watchdog (1024 MiB floor, logs the test's
working set when RAM goes low); 1280x720 window; copied saves; never touch Fixed Build; report measured
results separately from unverified ones.
