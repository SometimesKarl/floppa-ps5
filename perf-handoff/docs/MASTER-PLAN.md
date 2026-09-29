# ASTRO BOT on KytyPS5 — Master Plan (status document)

Updated 2026-09-28 17:50 (session 4). The governing plan is `OPUS-AUTONOMOUS-MASTER-PLAN.md` (same as
`DEVELOPMENT-PLAN.md` but with a sustained, well-paced 40 FPS target). This file records the state and the next actions;
`WORKLOG.md` has the per-run details. Resolution/shader workstream: `NATIVE-1080P-PLAN.md` (R0-R4 native 1080p, M1-M3 VRAM/transfers, S1-S3 shaders/per-draw CPU). User direction 2026-09-28 18:00: focus on Demon's Souls (performance, stability, shader and texture loading; fixes carry over to ASTRO BOT), plus an opt-in reduced texture-quality setting for VRAM. DS game folder is 164 GB (223k files) on S: (HDD); C: has 14 GB free, so moving it is not practical.

## 0. Session 4 state (read first)

| | |
|---|---|
| Source HEAD | **c837382** on `local/astro-perf-up` (clean). Deployed (Optimized Build): **78256f4**. |
| Staged, not deployed | `builds\a4d09c0-clangcl` (= 78256f4 + memo-validation order, memo bypass, draw-size stats, SRT delta stats; incremental walks reverted), `builds\c837382-clangcl` (+ band uploads for Standard64KB tiling, whole-upload reasons in KYTY_UPLOAD_LOG). Neither is verified on ASTRO BOT yet. |
| Verified this session | 78256f4 (X43, deployed). adb9a0c incremental walks: correct (3.08 M walks, 0 mismatches) but slower (walk 45% vs 34% of the GPU thread): reverted. |
| ASTRO BOT dune | Pure GPU-thread CPU ("other" 46-66 ms per frame, no gpu-wait): ~14k tiny draws/s; MaterializeResources 34%, ExecutePreparedDraw 27%, amdvlk 11-15%. No single item >35%: the next 2x needs parallel draw preparation (Phase F) or a compiled SRT (D2). |
| Demon's Souls | Fresh cwd boots, cinematic ~31 FPS, character creation 2-19 FPS, tutorial gameplay reached (S12) at ~3.5 FPS: ~5k indirect draws per frame, frame ~290 ms = other 200 + gpu-wait 60 + buffer 20. Game files are on S: (5400 rpm HDD, loose files): first-time path lookups (APR ResolveFilepathsToIdsAndFileSizes) wait on disk seeks. RAM: working set 10.2 GB in gameplay. |
| Opt-in settings | `emulator-settings.ini`: `render_resolution` (1080p set), `frame_cap=off|30|20` (off; user runs 190 Hz). Env: KYTY_ASYNC_PIPELINES=1, KYTY_PARTIAL_UPLOADS=0, KYTY_FRAME_CAP, KYTY_SRT_MEMO_STATS, KYTY_UPLOAD_LOG. |
| Rejected with measurements | GPL fast link; spirv-opt; branchless guarded loads (driver-cache invalidation); VS/PS walk parallelism (~10% max); incremental SRT walks (slower). |
| Test tools | drive.py (DRIVE_TEMPLATE / DRIVE_GAME / DRIVE_PATCH=none / DRIVE_NO_VALIDATION=1), route_desert.sh, travel.sh, ram_watchdog.py 1024 (logs the test's working set when RAM goes low), windows10.py (FPS per 10 s), stack_sample.py + stack_report.py, thread_sample.py (THREAD_SAMPLE_EXACT=1), guest_disasm.py (disassembles guest code of the test process). |
| RAM needs | ASTRO route ~10.2-10.9 GB working set: launch with >= 10.6 GB available. DS: ~10.3 GB in gameplay. |
| Next actions | 1) S13: DS with c837382, compare upload volume with S12 (cinematic 52 GB/10 s). 2) ASTRO desert + dune on c837382 (visual check: striping in the dark intro scene still unattributed). 3) Deploy c837382 if both pass. 4) Per-draw CPU: prototype prepare/record split or walker overhead cuts with a verify mode. 5) DS: fewer metadata syscalls per APR path (one GetFileAttributesEx). |

## 1. Where things are

| What | Where |
|---|---|
| Source | `C:\Users\himav\Desktop\kyty ps5-src`, branch `local/astro-perf-up`, HEAD see §0 (upstream merged to 421684e). |
| Build | Commit first, then build. A build from uncommitted source disables the driver pipeline cache ("disabled (dirty build)") and makes every test compile cold. PowerShell: `& cmd.exe /c "\"$b\build-clangcl-release.cmd\" clangcl-release > \"$b\build-<sha>.log\" 2>&1"` with `$b = ...\_Build`, then `tools\stage_build.ps1 <sha>`. The Qt launcher is not built here (no Qt), so new settings go in `emulator-settings.ini`. |
| Deployed | `Optimized Build\` = **78256f4** (backup kyty_emulator.4463b70.exe), with `emulator-settings.ini` set to `render_resolution=1080p`. Backups: `kyty_emulator.cad952f.exe` (previous), `.ec00b61.exe`, `.prev.exe` (9ae60ef). `Fixed Build` is never touched. |
| Test folder | `testbed\desert-warm` = the desert save plus a warm pipeline cache and recorded shaders/pipelines. Copy it fresh for each run: saves that the game changed in earlier runs altered the route. |
| Notes | `WORKLOG.md` (session log), this file. |

### Test rules (learned the hard way)
1. **Never kill emulators by name** (`taskkill /IM kyty_emulator.exe`). The user may be playing at the same time. Close only the test pid (`drive.py close`, or `taskkill /PID`). `tools\ram_watchdog.py` now kills only the pid in `drive-state.json`.
2. **Windowed 1280×720 only** (the captured launcher command line). No 1920×1080 test windows.
3. **Run the RAM watchdog during every test** (`ram_watchdog.py 450`). The PC hard-froze once in Sky Garden at 170 MB free.
4. The user's open apps take ~8 GB of RAM, and ASTRO BOT needs 7–10 GB. Check free RAM before a test (the A1 run at 170 MB free ran at 12 FPS instead of 30).
5. `route_desert.sh` sometimes misses the title screen (the game skips straight to the desert). Check `route.log`, then drive with `travel.sh`.

### Tools added this session
| Tool | Use |
|---|---|
| `KYTY_HITCH_LOG=1` | Each GPU-thread frame over 45 ms, split into: shader, pipeline, texture, buffer, gpu-wait, ring-wait, guest-idle, flip-wait, other. Plus a 10 s summary. |
| `KYTY_UPLOAD_LOG=1` | Textures uploaded again and again, and which GPU writes (fill/copy/storage) marked them. |
| `KYTY_MEMORY_STATS=1` | Adds texture age (<1 s/<5 s/<30 s/older), formats and shared addresses. |
| `KYTY_GPU_STATS=1` | Adds the most-bound color-target sizes, which show the game's current render resolution. |
| `KYTY_COLOR_CLEAR_LOG=1` | Targets that still drain the GPU for a clear check. |
| `tools\travel.sh [rounds]` | Moves Astro: run, turn, jump, attack (about 25 s per round). |
| `tools\window_fps.py` | Average FPS, p50/p90/p99, 1% low and worst frame over a time window. |
| `tools\measure_usage.ps1` | GPU 3D %, VRAM, CPU %, RAM of the test pid. |
| `tools\resident_breakdown.py` / `ws_growth.py` | What is resident in RAM (guest vs host) and where it grows. |
| `tools\sample_lines.py` | IP samples by source line. |

---

## 2. Measured state (2026-09-28, desert, windowed)

| | Value |
|---|---|
| Desert, 1080p mode | ~37 FPS average, frame p50 31.5 ms, p90 34 ms. But p99 64 ms and worst 187 ms: the spikes are the stutter. |
| Desert, native 4K (game at 3840×2160) | 22.6 FPS, p50 42 ms (GPU-bound) |
| Travelling (X31) | Mostly smooth, but some areas run 60–90 ms frames for 5–15 s at a time. See §4.2. |
| Repeat visit (warm) | 0 shader or pipeline stalls: 253 shader lookups and 234 pipeline lookups at ~0 ms (X28). |
| Startup precompile | 251 shaders in 4.8 s, 238 pipelines (227 from the driver cache, 11 compiled) in 3.8 s |
| First visit, Sky Garden (E39) | 430 pipelines, 124 s of driver compile (186 over 100 ms, up to 3.1 s each), plus 481 shader translations (9 s). The GPU sits idle meanwhile. This is the level-entry freeze. |
| VRAM, Sky Garden (E40) | 6.89 of 6.89 GB used (100%). Textures 4.75 GB in 1,205 images, all used within 1 s; 550 MB of GPU buffers spilled to system RAM; 3.9 FPS. The PC then froze. |
| RAM | The emulator's own memory is ~0.9 GB. The game's memory is 6 GB at the title and 9.4 GB after 10 min (its 4.8 GB pool at guest 0xFC_0000_0000 fills ~2 MB/s). |
| Output resolution | The game always registers 3840×2160 display buffers and ignores the reported output resolution (all values 0–4 tested). |
| Dynamic resolution steps | The main passes run at 1920, 2432, 3328 or 3840 wide. The game picks the step from the GPU timestamps the emulator writes (see §3). |

---

## 3. Done (session 2, 2026-09-28)

| Commit | What | Effect |
|---|---|---|
| d6511aa, fb12064 | Reverted e02f016/5e428ba (A/B from the traces showed no gain) | Simpler code |
| 0f58928 | Skip the color-metadata readback when no code can decode (it drained the whole GPU once per frame) | Desert ~30 → ~38 FPS; GPU-thread wait samples 15–22% → ~4.5% |
| e9856e3 | An image that doesn't fit reclaims idle images and retries (no exit). Smart async pipelines: a draw is deferred only when all its targets were drawn in the last 2 frames; otherwise it waits (sand bug impossible). | Cold desert: 124 pipelines (79 s of compiles) moved off the frame |
| fce40f0 | Pipeline prewarm (`.pipelines` file, rebuilt on all cores at boot); VRAM headroom policy; texture age stats | Repeat visits: no pipeline stalls even after a driver update |
| 9b366c4, f064be6 | `emulator-settings.ini` `render_resolution=auto\|1080p\|1440p\|2160p`: steers the game's dynamic resolution through the timestamp clock, holding a step near the target (steps switch = render-target recreation hitch) | 1080p mode: 37 FPS vs 23 at 4K |
| d102134, bb20c15, 55ebe6d, 4463b70 | Hitch log, upload log, invalidation tally; texture collector no longer reacts to low system RAM (that evicted in-use textures: 8 GB re-uploaded in 10 s at 2 FPS) | Diagnostics; fixed a self-inflicted 2 FPS state |

Experiments ruled out:
- `VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION`: made compiles 15× slower and lost the device.
- Reporting a lower output resolution: the game ignores it.

---

## 4. Root causes found (ranked by what the user feels)

### 4.1 Level-entry freeze (GPU nearly idle)
- **What happens:** every shader and pipeline of a level seen for the first time is translated and compiled on the GPU command thread, one after another. At level entry every render target is new, so the smart async rule can't defer anything, and each draw waits.
- **Why it's so slow:** this game's shaders are huge, 90–180k SPIR-V words with EXEC predication, and AMD's compiler takes 0.1–3 s per pipeline. Sky Garden: 124 s of compiles.
- **Current mitigation:** the second visit is instant (replay plus prewarm).

### 4.2 Frame drops after actions or in some areas
- **Render targets copied back and forth every frame.** The game writes render-target memory through storage buffers (transient memory aliasing). Every such write marks the image "buffer modified", and the whole image is re-uploaded from the buffer on its next use.
  - Size: 15–23 GB per 10 s (1.5–2.3 GB/s), the same 2432×1368 and 1920×1080 RGBA16F targets every frame.
  - The write ranges covered only ~27% of the image bytes marked.
  - Costs up to 12–13 ms of GPU-thread time per frame, plus upload-ring pressure.
- **Two dynamic-resolution sizes of one target coexist at overlapping addresses** (0x53AA00000 at 2432×1368, 0x53AD00000 at 1920×1080). Both are re-uploaded.
- **Heavy stretches** (X31 at 366–381 s): gpu-wait 25–33 ms plus "other" (draw processing) 25–35 ms per frame. The ring-wait split (4463b70) will show whether the waits are upload-ring waits; not measured yet.

### 4.3 VRAM on 8 GB cards
- Sky Garden's textures alone are 4.75 GB, and all 1,205 images are "used" within 1 s. The game binds its material tables every frame, so age-based eviction can't find anything to free.
- The Windows budget shrinks when other apps use VRAM (7.35 → 6.89 GB).

### 4.4 RAM on 16 GB PCs
- The game's own memory is 7–10 GB. With ~8 GB of other apps open, the PC runs out.
- Evicting textures doesn't help RAM; only closing apps does.

### 4.5 Frame pacing
- The emulated 60 Hz vsync makes a 25–35 ms frame show for either 33 or 50 ms, which reads as judder. There's no steady-cadence mode yet.

### 4.6 CPU cost per frame (unchanged from session 1)
The GPU command thread spends ~25–30 ms per frame:

| Item | Time per frame |
|---|---|
| MaterializeResources | 7–9 ms |
| PrepareBindings | ~3 ms |
| RELEASE_MEM handling | ~4 ms |
| Submits | ~1.4 ms |
| Draw setup | the rest |

---

## 5. Plan: stutter first

| # | Fix | Why / expected | Size |
|---|---|---|---|
| **S1** | **Graphics pipeline library (VK_EXT_graphics_pipeline_library), DXVK-style.** Compile each shader stage into a library on a worker as soon as it's translated. Link pipelines without link-time optimization (~1 ms), use the linked pipeline right away, and compile the optimized one in the background and swap it in. Needs support on the RX 6650 XT driver; fall back to today's path if missing. | Removes the level-entry freeze and first-use stalls without skipping draws | Days |
| **S2** | **Parallel shader translation.** Run the recompiler for new shaders on workers. At level load, pre-translate every shader referenced by the loaded level's draws. | The remaining 9 s of translations per new level | 1–2 days |
| **S3** | **Shader/pipeline packs.** `.shaders` and `.pipelines` are portable. Keep one master set recorded across the whole game and replay it at boot, so first visits behave like repeat visits. | Cheap. Needs a playthrough (the user's own play fills it automatically) | Hours |
| **S4** | **Stop the render-target round trips.** In order: (a) don't re-upload an image whose next use is a color target fully cleared or overwritten before any read; (b) upload only the byte range the storage write covered; (c) one image per render-target memory for all dynamic-resolution sizes (a view per size, not separate images). Measure with `KYTY_UPLOAD_LOG`. | 1.5–2.3 GB/s of copies; up to 12 ms per frame in bad areas | Days |
| **S5** | **Steady frame pacing option:** `emulator-settings.ini` `frame_cap=30\|40\|45\|off`. Present at a fixed cadence: an emulated vblank of 90/120 Hz plus a flip-rate lock, or a present-time limiter. Test with FreeSync. | "45 steady beats 60 swinging" | 1 day |
| **S6** | **Hitch hunt with the new logs:** travel tests in the desert, galaxy and Sky Garden. Split ring-wait vs gpu-wait vs other, fix the top cause each time, and report p99, 1% low and worst frame. | Measurement loop | Ongoing |
| **V1** | **VRAM: evict by what is sampled, not just bound.** Find the textures the game binds but never samples (bindless tables). Evict them first under pressure; re-upload on first real use. | Sky Garden fits in 8 GB with headroom | Days |
| **V2** | **VRAM: duplicates.** Images recreated for other view formats ("recreating a format 91 image"). Dynamic-resolution size variants (S4c). Stale GPU-written targets from the previous level: free them 5 s after a level change. | Hundreds of MB | Hours–days |
| **V3** | **VRAM: mip residency.** Upload only the mips the game has resident or samples (PS5 streaming); check `TryReadPrtBacking` paths. | Large on texture-heavy levels | Research |
| **C1** | GPU-thread CPU cost: D2 (compiled SRT programs / value-numbered memo), C4 (skip unchanged descriptor rebinds), then **D1**: a second thread that records Vulkan commands. | 25–30 ms per frame; needed for 45+ FPS | Days–weeks |
| **C2** | Barrier/render-pass narrowing (D4), real occlusion queries (D5), texture-cache interval index (D6), persistent translated-shader cache (D8). | From session 1's plan | — |
| **R1** | True emulator-side resolution scaling (render every screen-sized target at 1080p, including the game's 4K output passes). Needs viewport/scissor, integer-coordinate and dispatch-size scaling in the recompiler. High visual-bug risk. | Only after S1–S5 | Weeks |

**Order next session:**
1. S3 (cheap)
2. S6 measurement with ring-wait
3. S4a/S4c
4. S1 (GPL)
5. S5
6. V1/V2
7. C1

---

## 6. Resolution notes
- **What 1080p mode does:** the setting steers the game's own dynamic resolution. 1080p mode pins the main passes to 1920×1080. The game's final 4K image (anti-aliasing output and UI) is still built at 3840×2160 by the game, then scaled to the window.
- **1440p mode:** maps to the game's 2432×1368 step.
- **Upscalers (FSR 1/NIS in the presenter):** only help once the output passes themselves are smaller (R1). The display buffer is always 4K today, so an upscaler would only be downscaling.
- **Real GPU timestamps:** writing actual GPU times (VK_EXT_calibrated_timestamps) instead of GPU-thread recording times would let "auto" follow real GPU load. Needed for a good auto mode.

---

## 7. Research items
- GPL support and fast-link cost on the AMD 6650 XT driver; DXVK's GPL design (shader libraries, background optimize, pipeline swap).
- ASTRO BOT's transient aliasing: which passes write render-target memory through storage buffers, and whether the next use of those images is always a full overwrite (S4a correctness).
- ASTRO BOT's bindless material tables: how to tell "sampled" from "bound" (V1).
- The E33 12-minute softlock and the Thread_Gpu crash on shutdown (from session 1, still open).
- Sky Garden walk with the current build and the pid-only watchdog. The headroom policy is untested there after the freeze.

---

## 8. Milestones

| Milestone | Exit criteria |
|---|---|
| M1 Stable | 30 min through 3 levels: no crash, no PC freeze, VRAM ≤ budget − 0.5 GB |
| M2 No stutter | First visit to a new level: no frame over 250 ms after the loading screen. Repeat visits: no frame over 100 ms. Travel tests: p99 ≤ 50 ms. |
| M3 Steady 30/40 | frame_cap mode holds the cap with 1% low ≥ cap − 5 in desert and Sky Garden |
| M4 Steady 45+ | GPU-thread cost ≤ 20 ms per frame (C1) |
| M5 Low-end | R1 or 1080p output passes; 8 GB VRAM with headroom |
| M6 Fidelity | Lighting/ray-tracing PRs, occlusion, particles |

## 9. First steps next session
1. Start the RAM watchdog (pid-only). Copy `testbed\desert-warm` fresh for each run.
2. X34 redo: travel test with `KYTY_HITCH_LOG=1 KYTY_UPLOAD_LOG=1`, 1080p mode. Read ring-wait vs gpu-wait vs other.
3. S3: gather the `.shaders` and `.pipelines` from the user's `Optimized Build\_PipelineCache` after they play; keep a merged master copy.
4. S4a/S4c prototype, then remeasure uploads per 10 s and p99.
5. S1 design: query GPL support, sketch the library cache keyed by SPIR-V hash plus stage state.

## Isolated native-resolution checkpoint — 2026-09-28

- Separate research worktree `NATIVE-1080P-ISOLATED\\source`, branch `codex/native-1080p-research`, is checkpointed at `569d146`; the matching clean Release candidate is `NATIVE-1080P-ISOLATED\\builds\\resolution-observe-no-steer-569d146` (SHA-256 `19EDD8FD2A638F203F437C40B2AD35002D68B53FC75EE200A8E40DD715E41F20`). No deployment was made.
- New width-attribution trace showed screen-shaped color-target bind winners oscillating among 1920, 2432, 3328 and 3840 in startup. The previous clock steering is now off by default in the isolated candidate and requires `KYTY_LEGACY_DRS_STEERING=1`; the old reversed candidate is not a fix.
- Runtime still reports a 1080p output hint while ASTRO registers two 3840x2160 buffers. Native 1080p remains unresolved. The short no-steer boot stopped at shader loading, before menu/gameplay. A separate boot was stopped by its exact-PID watchdog at the 3 GiB free-RAM floor.
- Do not launch the dune route until available RAM is at least 10.6 GiB. Existing X47 dune evidence remains CPU-command-preparation heavy (18.8–21.7 FPS, p99 70–72 ms); GPU busy is not measured. Next: scene-resource identity tracing, then one bounded, matched dune run with CPU samples and GPU timestamps.

## Latest isolated resolution candidate — 2026-09-28

- Source checkpoint advanced to `fc569fa` in `NATIVE-1080P-ISOLATED\\source`. Candidate `builds\\resolution-target-identity-fc569fa` has SHA-256 `64B6098E2B63BA454895E811C900C24492D0906600078CC782441F9397185DA6` and adds opt-in target identity histograms; not deployed.
- Latest logger is compile-verified only. No gameplay test was run because available RAM remains below the 10.6 GiB safety gate. The current 1080p status hint still does not equal native 1080p; the earlier runtime trace registered two 3840x2160 guest display buffers.

## Isolated continuation — shader diagnostics and dune telemetry (2026-09-28)

- Separate workspace remains under Performance Experiments\NATIVE-1080P-ISOLATED; its source is branch codex/native-1080p-research at commit 4976070. The matching clean Release candidate is builds\shader-lookup-profile-4976070\kyty_emulator.exe (SHA-256 540B5251881E437EEBFD69B7187F5CE51B945729F111E7C2CAE18FBB76130395). It is not deployed.
- Native 1080p remains unachieved: the verified runtime still had a 1080p mode hint alongside two 3840x2160 registered guest display buffers. Do not resize guest registration or claim a native mode until the scene/composition-to-flip path is mapped.
- Added profiling-only CPU scopes for GetDeclaredShaderHash and GetShaderParams to expose shader-input work below PrepareProgram. Build passed; runtime validation is pending. No shader results, caches, clock values, or render behavior changed.
- Prepared an isolated, exact-PID Windows usage sampler. Syntax and unrelated-PID refusal checks pass; Windows GPU Engine and process-memory counter sets enumerate. Tracy Vulkan GPU zones are available with --profile but add attribution overhead; the guest reference-clock packet path still supplies host-TSC values, not actual GPU execution durations.
- Available RAM was 9.67 GiB at the latest safety check, below the 10.6 GiB threshold. No dune/transition run was launched and no new GPU-usage result exists. Existing X47 CPU profile remains the only dune evidence (18.8–21.7 FPS in selected windows; 70–72 ms p99; GPU busy unmeasured).
- Next: when RAM is actually at least 10.6 GiB and no user emulator is active, capture one short 1280x720, frame-cap-off dune interval with the exact-PID sampler; use a separate short Tracy profile interval for child-zone attribution. Then correlate traced target addresses and final VideoOut dependencies before attempting native-resolution changes.
- The active source/build, Fixed Build and Optimized Build were not modified.

## Isolated compute work card — 2026-09-28

- New external lead: Prosper issue #1732 reports a compute-path throughput bottleneck for ASTRO BOT in that separate emulator, with the mechanism explicitly unresolved. This is hypothesis material, not Kyty evidence: https://github.com/mattias800/prosper/issues/1732. Keep compute enabled.
- Isolated Kyty commit 9bfa43f adds profiling-only scopes around DispatchDirect, DispatchIndirect and PipelineCache::GetComputeProgram, preserving the shader lookup scopes from 4976070. Clean one-worker Release candidate SHA-256: 59D1DF8E69C748020BF902C932EEFF16376A6C2EAF68A11C9A299EA7D5C5D7FE. It is staged in NATIVE-1080P-ISOLATED and not deployed; runtime validation is pending.
- Compare these CPU scopes against existing Tracy GPU guest-dispatch zones and the exact-PID process 3D counters. This will show whether dispatch preparation, queue submission, or device execution dominates the sampled scene.
- Latest free RAM: 9.26 GiB, below the 10.6 GiB route gate. No game launched. Native 1080p remains unachieved and the previous 3840x2160 guest-buffer evidence still applies.
- Next: once at least 10.6 GiB is actually free and no user emulator is active, run one short windowed 1280x720 dune interval with frame cap off and the owned-process sampler; take a separate brief Tracy profile interval for zone attribution. Then return to scene/composition target mapping.

## Scanout range mapping — 2026-09-28

- The latest bounded boot-only run stopped before the title menu: it was still on the opening cave cinematic. One titlebar observation was about 45 FPS and must not be treated as desert gameplay performance. No GPU-busy or dune result was collected.
- Presenter tracing showed 3840x2160 guest input/backing/presenter extents and a 1280x720 host drawable. The run does not prove native 1080p rendering or 1080p host output.
- Runtime again reported a 1080p output hint with a 1280x720 host context, but ASTRO registered two 3840x2160 guest display buffers. Screen-shaped color-target bind identities spanned 1920x1080, 2432x1368, 3328x1872 and 3840x2160; the current trace cannot classify primary scene, temporal history, post-process, UI, or scanout resources.
- Offline top-six-per-window counts place 3840x2160 identities in 135 windows with 8,671 reported binds; 3328x1872 in 50/2,628; 2432x1368 in 16/6,538; and 1920x1080 in 32/2,407. This is a truncated histogram. Frequent candidates `0x0520440000` (3840) and `0x05168c0000` (2432) still need address-range matching and producer/consumer tracing before assigning scene/output roles.
- Format analysis suggests the two adjacent recurring 3840x2160 format-58 images (`0x0507410000`, `0x05093f0000`, each `0x1fe0000` bytes) are the double-buffered VideoOut surfaces: format 58 is A2R10G10B10 UNORM, matching ASTRO's registered VideoOut format. Exact guest registration/flip-address match is pending. At roughly 63.8 MiB combined, these buffers alone cannot explain multi-GB VRAM pressure. Format-97 surfaces are 64-bit R16G16B16A16_SFLOAT, repeated at all DRS-like sizes and are more plausible scene/HDR cost centers, pending role attribution.
- The isolated `render_resolution=1080p` currently sets an observation preference only. With legacy reference-clock steering off, it does not change guest resolution. The 1080p hint follows host context and ATTRIBUTE3 detection; it is not evidence of native internal or guest scanout resolution.
- An isolated, bounded VideoOut trace now reports registered buffer address/range and first 32 actual flip source ranges. Runtime matching confirms the two registered 3840x2160 buffers (`0x0507410000`, `0x05093f0000`) are the recurring format-58 color targets; the first 32 flips alternate between them. Candidate `NATIVE-1080P-ISOLATED/builds/scanout-range-trace-9bfa43f-dirty/kyty_emulator.exe`, SHA-256 `D03B78857EADA3B3DB240F19E971ABD32E4D3744809544C67C6B44F4A5B6CF07`; boot trace validated, no gameplay/performance validation, not deployed.
- An earlier boot reached 6254 MiB working set and was watchdog-stopped. The new scanout run was gracefully closed on exact PID 2580 at 3634 MiB available after ~37 seconds, then RAM recovered to 8.92 GiB. Its hidden watchdog failed to start because paths with spaces were unquoted; corrected quoting was verified against the exited PID. Latest free RAM remains below the 10.6 GiB gameplay gate. No transition or GPU-use test was performed. Continue offline path analysis; when the gate is met, measure dune with sampler-only before a separate brief Tracy attribution pass.
- The isolated launch helper now starts the owned-process watchdog automatically (default 3072 MiB; minimum configurable threshold 1536 MiB) and writes its log inside the run folder. The subprocess argument-list path was smoke-tested against the exited PID; it reported a safe exit. No emulator runtime code changed for this safeguard.
