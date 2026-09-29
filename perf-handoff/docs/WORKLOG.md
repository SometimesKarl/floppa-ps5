# Work log / state for continuing the ASTRO BOT optimization (read this first after a context reset)

User goal (2026-09-26): keep implementing `OPTIMIZATION-PLAN.md` autonomously toward 60 FPS; use CPU cores/threads and VRAM
efficiently, not only RAM. User is AFK and granted broad authority. Never touch `Fixed Build`; ship via `Optimized Build`
(keep previous exe as `kyty_emulator.prev.exe`). Never claim FPS gains without the harness A/B. Correctness gates in plan §2.

## Paths
- Source: `C:\Users\himav\Desktop\kyty ps5-src`. CURRENT branch `local/astro-perf-up` = our commits rebased onto upstream
  origin/main 16b83a0 (22 new upstream commits, clean rebase). Old branch `local/astro-perf` (base 7855c98) kept intact.
  `local/upstream-baseline` = origin/main + 2 boot fixes only (shader validation 1db9992, longPathAware 1bedf87), for A/B.
  Build dirs: `_Build\clangcl-release` (ours), `_Build\clangcl-upstream` (baseline). Build identity has no perf effect.
- Build: PowerShell: `& cmd.exe /c "`"$b\build-clangcl-release.cmd`" clangcl-release > `"$b\<log>`" 2>&1"` with
  `$b='C:\Users\himav\Desktop\kyty ps5-src\_Build'`. Output `_Build\clangcl-release\kyty_emulator.exe`, needs
  `libwinpthread-1.dll` from same folder. Incremental ~3 min; full ~15 min. (cmd //c from bash fails on the space in path.)
- Stage: `Performance Experiments\builds\<shortsha>-clangcl\` (exe, pdb, dll, build-manifest.txt).
- Harness: `Performance Experiments\tools\` (copy into each new experiment folder):
  - `run_experiment.py --label L --exe EXE --cwd DIR --out OUT --capture-at 180 --windows 3 --window-s 60 [--no-gpu]
     [--add-arg=--profile --tracy-seconds 10] [--env K=V] [--set-arg --name value]`  (use `--add-arg=--profile` form).
  - `run_batch.py batch.json` (fresh cwd per run from `testbed\installed-exe`), `compare_runs.py out.json name=RUN,RUN ...`
  - Tracy: `tools\tracy-0.14.1\`; analysis `tracy_nesting.py RUN 50`; zone CSVs `tracy-zones.csv`, `tracy-zones-self.csv`.
- Scene A: title screen, capture from 180 s. Scene B: opening cinematic 20–110 s (builds reach stages at different times).
- Baseline exe for A/B: `Fixed Build\kyty_emulator.exe` (GCC, 15.4 FPS) and current best `builds\fb3f8a3-clangcl` (~18.8).

## Pitfalls learned
- Never run builds while measuring. One thing at a time.
- Python byte-literal edits: `\\n` inside printf strings became real newlines twice; verify with sed after editing C strings.
- Files are CRLF; edit with Edit tool or bytes-preserving python (read/write 'rb'/'wb', replace b'\n' with b'\r\n').
- Tracy `FrameMark` must be guarded (`Profiler::MarkFrame`) — manual lifetime.
- The game asset paths exceed MAX_PATH: manifest `longPathAware` is required (commit 1037d11).
- Window resize/move by the user corrupts runs; orchestrator restores 1280x720 before each window.
- User activity (RAM drops) contaminates runs: check `avail_mib`/hard faults in results.

## Results so far (title screen, E05): installed 15.40 FPS -> clang clean 15.78 -> e289092 18.82 (+22%), p99 92->70 ms.

## Status of plan items (updated 08:45)
- DONE + committed: d0b65c3 P0.1 DCC check dedup (250->6 checks/frame); ee39b36 P0.2 Tracy GPU zones
  (GPU work ~22-24 ms/frame; draws ~16 ms); 7d1eac6 P1.1 async guest readbacks (GPU-thread drain 14.6->0.14 ms/f);
  a22ff0c P2.1 marker-based EOP interrupts (submits 341->34/frame).
- Measured: dirty wip-p21 (= a22ff0c code) 30.4 FPS title (E11 N1, frames vblank-quantised 16.7/33/50 ms);
  clean a22ff0c measured 26.4 FPS in E11 AB1 (both A and B variants) -> investigating (E11 AB2: dirty vs clean identity).
- Uncommitted in tree: textureCache.cpp relax DCC texture guard (reason 8, non-GPU-owned texture binds). A/B: no effect
  (26.42 vs 26.37). Decide keep/revert.
- Remaining CP drains (GPU thread): DispatchDirect -> ReadMemory (DCC texture path + PrepareProgram reading GPU-dirty
  shader code at cs data_addr), DrawIndirect args (P1.2). GPU draws ~16 ms/frame are next wall for 60 FPS.
- Optimized Build still = fb3f8a3 (18.8 FPS). Update after confirming best build.

## 2026-09-26 18:00+ session (after context reset)
- E11 AB2: dirty wip-p21 26.31 vs clean a22ff0c 26.55 FPS -> identity/pipeline cache has no effect; N1 30.4 was an outlier.
- E12 diag (patch: E12-drains/diag-writes.patch). Per-frame GPU-thread drains were:
  (a) false sharing: CS 0x5006eac00 writes 16 B at 0x50740a520/540 on the same 4 KiB page as CS code at 0x50740a700,
      so the PrepareProgram header read faults and drains; (b) DrawIndirect args at 0x56e35a7e0 written by CS
      0x500704800/0x500702f00 (true dependency); (c) no DCC fallbacks with the relaxed guard. Removing one drain alone
      shows no FPS gain because the next drain in the frame absorbs the wait, so all must go.
- New commits on local/astro-perf-up: 1e4c658 shader header via GPU-clean backing; 5475d3e in-flight read-backs count
  as dirty (race fix for f6fb071/7d1eac6); 25be3c2 relaxed DCC guard; 7b1645d GPU-side DRAW_INDIRECT (non-NGG, non-quad).
- Build 7b1645d staged in builds/7b1645d-clangcl. Upstream baseline built in _Build/clangcl-upstream.
- Next: E13 A/B a22ff0c vs 7b1645d vs upstream baseline; screenshots for correctness; Tracy trace of 7b1645d.

### E13 results (19:00-20:10)
- Same session, title screen: upstream f9eb4c0 (16b83a0 + 2 boot fixes) 16.0 FPS; a22ff0c 28.65; 7b1645d 28.4 (N run
  invalid: user pressed a button, game went to the desert level).
- Title screen is at the 30 FPS vsync step: game has 2 flip buffers, never sets flip rate, GPU work ~24 ms/frame, so
  ~86-90% of frames take 2 vblanks. Gains beyond 30 need a whole frame under 16.7 ms.
- T2 trace (7b1645d): shader-page drain gone but 5475d3e window-wide in-flight check made the header read wait anyway
  (fixed by 8dfa7a4, exact byte ranges). DrawIndirect drain remains: it is DRAW_INDEX_INDIRECT on an NGG (mesh) draw,
  stages=0x2030, INDEX_BUFFER_SIZE=1, point list, args 0x56e35a7e0. Mesh draws push draw params as push constants,
  so a GPU path needs a recompiler change (EmitMeshDrawParameter is the single load site) + conversion compute pass.
- LOGF is silent in these runs (Log::IsSilent); use std::printf for diagnostics that must show.
- 9ae60ef (8dfa7a4 exact ranges + indexed GPU args + flip-rate print): paired A/B vs a22ff0c: 29.8 vs 26.7 and
  30.39 vs 26.56 (+11-14%). Deployed to Optimized Build (prev exe kept as kyty_emulator.prev.exe).
- Game prints: "game uses occlusion queries, treated as always visible" -> GPU does work the game would cull.
  V7 patch applies cheat "Disable GI probes and lighting shaders" (user baseline, leave as is).
- Next GPU-side candidates: (1) drop per-dispatch emulator barriers, rely on guest CS/PS partial-flush + ACQUIRE_MEM
  events which already call EmitGlobalBarrier; (2) real occlusion queries; (3) NGG indirect on GPU.

### 2026-09-26 late / 09-27 (E14)
- Render-target sizes (title): G-buffer 1920x1080 x5 MRT, 2432x1368/1216x684 intermediates, 3840x2160 output, bloom chain,
  1024^2 shadows. Internal res already 1080p; not a resolution problem.
- All color images were MUTABLE_FORMAT without VkImageFormatListCreateInfo -> AMD disables DCC. Same-binary A/B with a
  format list {format, sRGB partner} (env experiment): 28.9 -> 32.1 FPS (+11%, CI +6.5..+15.4%), static regions identical.
  Productionized as e464a1a (FindImage recreates the image unrestricted when a view format falls outside the list).
- 20c81f6: controller hot-plug no longer fatal (SDL_OpenGamepad failure crashed a run).
- f149b3d: PrepareBda scans only when the tracker's CPU-dirty generation moved (~76 scans/frame before).
- Staged (uncommitted): WaitForMarker polls with timer backoff instead of 100 us vkWaitSemaphores (unnamed thread at
  ~88% of a core is most likely this), thread named Thread_GpuPriority.
- VRAM 4.25 GB dedicated, 0.7 GB shared, RAM WS 7.6 GB / private 5.9 GB at title (tools/sample_gpu_memory.ps1).
- Harness: run_experiment.py --key-at SECONDS:KEY (J=Cross, RETURN=Options) posts key presses for gameplay scenes.
- E14 AB (e464a1a vs 9ae60ef) was interrupted by the user; rerun as AB3 (P=9ae60ef, N=e464a1a, B=f149b3d).
- Candidates next: re-tiling redundancy (SynchronizeBufferFromImage 12/frame ~1.2 ms GPU), occlusion queries,
  per-dispatch barriers, async submit (~2.7 ms CP), NGG indirect (last per-frame drain), readback latency.

### 2026-09-27 00:40-01:15 (E15)
- Test contamination sources found and fixed in the harness: the emulator window grabbed keyboard focus (user typing
  moved runs off the title) -> EnableWindow(FALSE) once the window exists (--allow-input to opt out); a connected
  controller also drove the game -> test instances set SDL_JOYSTICK_*/SDL_XINPUT_ENABLED=0. Verify scene via luma ~60.
- Crash seen once (9ae60ef, AB3/P, free RAM ~420 MB, many user apps open): guest table scan
  `cmp [rdi+r8*4],ecx` with r8=0x584e9b2b ran past mapped memory at 0x11E8000000 (unhandled AV, runtimeLinker.cpp:719).
  Guest RAM is a pagefile-backed SEC_COMMIT section, so low RAM pages rather than fails. Watch item.
- Memory: host-private ~5.9 GB = 512 MiB upload staging + ~30 x 128 MiB write-combined (0x404) blocks + heaps;
  guest-private only ~0.4 GB. Ours ~6.3-6.6 GB private at ~200 s vs upstream ~4.5 GB; growth ~2-4 MB/s (ours) vs
  ~1.2-2 (upstream) scales with frame rate. Tools: tools/memory_map.py, tools/inspect_blocks.py.
  Commit 3f5c7ec adds KYTY_MEMORY_STATS=1 (per-memory-type VMA totals every 30 s) to attribute the blocks.
- 4ed523b fixes f149b3d: registering a buffer must advance the CPU-dirty generation (else PrepareBda could skip
  uploading a new BDA buffer's CPU data).
- tools/long_frames.py on T3: long frames (>=45 ms) have +3.9 ms DrawIndex CPU and +2.7 ms GPU waits (NGG
  DrawIndirect drain +2.1 ms) on the GPU thread.
- NGG DRAW_INDEX_INDIRECT on GPU: patch in scratchpad ngg_indirect.py (applied to tree, not built yet): mesh prologue
  reads args via GetAddressResource when push word3 bit31 is set; CPU builds {groups from INDEX_BUFFER_SIZE bound,
  instance count copied on GPU, 1} for vkCmdDrawMeshTasksIndirectEXT.
- Barrier experiment scripts (scratchpad exp_barriers*.py): KYTY_EXP_NO_DISPATCH_BARRIERS skips per-dispatch
  barriers; ACQUIRE_MEM (currently a no-op!) and CS_DONE releases then emit barriers if a dispatch ran since.

### 2026-09-27 02:00-02:30 (E17 soak, E18 level load) — plan v2 Package 1
- E17 soak (8 min title each): upstream f9eb4c0 U1-U3 and old base a22ff0c O1-O2: no crash. O2's 450 s shot was the
  attract demo (luma 149), not a crash or input.
- Adopted PR #838 (c0485c2: ReadFile into protected guest pages -> rewind + 1 MiB thread_local bounce buffer) plus a
  WriteFile mirror and a rate-limited stdout note (d90817e). 63f6877 adds KYTY_FRAME_LOG (per presentation: kind,
  request id, flip_arg, index, raw QPC of guest flip request / GPU completion / present begin / present end;
  kinds 2/3 = repeated/blank presentations).
- Harness: run_experiment.py sets KYTY_FRAME_LOG=<run>/frames-emu.csv, records each window's QPC range and
  summarizes it after exit (tools/frame_log.py). compare_runs.py rewritten: runs are the samples, invalid runs and
  windows excluded with reasons (--luma scene gate, --min-avail-mib), per-minute stats by captured duration, frame
  source emu (exact) or title (caption estimate). On E15 it now reports B2 (crashed) and M2 (never ran) as excluded.
- E18 L1 (63f6877, cold cache): the title's frame-log FPS matches the caption collector (33.03 vs 33.02). Title p50
  33.2 ms (vsync 2-interval step), GPU completes 12.5 ms after the flip request.
- ATTRACT DEMO: after ~2 min idle on the title (title appears ~67 s after a 17 s load stall at 48-65 s) the game loads
  the first level (desert, loading tunnel) and plays a demo by itself. Input-free, repeatable gameplay + level-transition
  workload. --key-at presses at 60/65 s fell inside the load stall and did nothing.
- During that level load the PR #838 retry fired twice ("File read hit protected guest memory"): those ReadFile calls
  would have failed silently before -> matches the user's crash on starting a new game. No crash in L1.
- Demo gameplay (cold shader cache): ~20 FPS, p50 ~48 ms; guest flip requests every ~43 ms, GPU done 1.6 ms after the
  request, ~8.5 ms vblank wait, hottest thread ~74%: a CPU<->GPU wait chain, not GPU throughput.
- RAM vs VRAM: emulator 4.8 GB dedicated VRAM + 0.7 GB shared, ~2.4 GB VRAM free during the level load. The ~5.6 GB
  of write-combined host-private blocks are only 12% resident (tools/block_residency.py, QueryWorkingSetEx) and match
  the dedicated VRAM: they are CPU mappings of VRAM, not RAM. RAM (WS 9.8-10.2 GB, free 0.5 GB during the load) is the
  guest's own memory (~8.6 GB resident) + ~0.9 GB host. VRAM is not the limiter; RAM headroom is thin by nature.

### 2026-09-27 02:30-02:50 (E18 L2 analysis, leak fix, Package 2 build)
- L2 (63f6877, no input, 20 x 30 s): title ~60-90 s, space flight (luma 46), desert level from ~150 s: Astro idles,
  luma 172-174 constant for 8 min (the game starts a new game by itself; not an attract demo). Desert: 18-20 FPS, p50
  50 ms; flip request never precedes the previous presentation (0/598); present->next request p50 35 ms, request->GPU
  done ~4 ms, vblank wait ~8.5 ms, present call 1-2 ms. Hottest thread ~50%, 2.2 cores.
- LEAK: private bytes grew ~1.5-2 MiB/s in the static desert (9.0 -> 10.0 GB in 8 min) with VMA flat. 72 heap
  segments of 15-16 MiB (NT heap header), full of 48-byte blocks whose payload is a SceKernelEvent {ident 0, filter -14
  (graphics), fflags, data 0xa/0xabcd}: InterruptEventTriggerFunc queues a copy per trigger in
  KernelEqueueEvent::pending_events (std::deque; MSVC stores one 32-byte element per heap block) whenever the event
  is still undelivered; the game waits far less often than EOP interrupts fire. Present since the initial commit
  (upstream too). Fix 03234d7: keep the newest 256 undelivered triggers per event (graphics + video-out; AMPR left).
  Tools: tools/heap_sample.py (dump/pattern a remote region), tools/block_residency.py.
- VMA (KYTY_MEMORY_STATS): device-local 4.1 GB at the title -> 5.6 GB in the level; ~200 MB of guest buffers landed in
  system RAM during the load (WITHIN_BUDGET fallback). 8c0a6aa adds heap budgets and per-owner totals.
- PR #793 cherry-picked (a1ebcdb..af9bfa7, conflict resolved keeping our GPU zone). 4437535: mesh DRAW_INDEX_INDIRECT
  with GPU arguments (MeshIndirectBuilder + shaders/mesh_indirect_args.comp: groups from the CPU bound, instances
  clamped to maxMeshWorkGroupCount[1] and maxTotal/groups, zero for empty draws, overflow flag -> CPU path with #793
  slicing; prologue clamps count to the bound past the first index so 32-bit address math cannot overflow).
- User started Optimized Build (9ae60ef) at 02:45:51 for ~1 min during E19 B1's boot: B1 is correctness-only for
  anything before ~130 s; its desert windows are gated by luma.

### 2026-09-27 06:30-07:05 (E19 rerun decision, E20-E23, 6f0d3f8)
- E19: only B1 completed (the user paused the rest; not crashes). B1 (4437535, leak + file fixes + mesh GPU args):
  private slope 0.36 MiB/s in the desert vs 1.80 (L2, 63f6877); private ~7.0 GB vs 9-10 GB; desert 21 FPS (1 run).
- a4a892e: the Vulkan pipeline cache file is keyed by vendor/device/driver/pipelineCacheUUID, not the build
  revision (every build discarded all pipelines: first-use stutter again after each update). 512 MiB bound.
- E20 (4437535 --profile, title; the new game did not auto-start this time): title 37-40 FPS. CP thread busy ~98%;
  it also blocked 3.1 s/20 s in ProcessCommands > Wait on guest-thread readbacks.
- E21 (user entered the desert with input; --profile): CP thread busy 42.4 of 43.4 ms/frame: DrawIndex 29.3 ms
  (497 draws, 14.6 ms of it unzoned self), ShaderApplyAttribSemantics 5.5 ms = ReadMemory full drains (2.5/frame,
  2.0 s/20 s of waits), PrepareBindings 4.9 ms, CommandScheduler::Wait 4.9 ms. Guest threads idle (cond waits).
- 6f0d3f8: vertex attribute/V# tables read through TryReadGpuCleanBacking (never reported as GPU-written in E22 =
  pure page false sharing, drains gone) + profiler zones along the draw path.
- Harness: --key-at SECONDS:KEY[:HOLD] from a background thread (also during windows), key_log with QPC;
  --wait-luma LO:HI scene gate (presses --advance-key every --advance-every s until two in-range screenshots,
  then --settle s). Route: presses of Cross skip the intro; desert reached ~150 s.
- E22 (6f0d3f8, unprofiled, route exploration): desert 22-29 FPS, p50 33.5 ms (2 vblanks) vs 50 ms before (1 run).
- Sync inventory (E22, 213 s incl. title/loads): ACQUIRE_MEM ~1.05M (all no-ops today), RELEASE_MEM ~3.5M
  (event 0x28 1.3M + 0x14 1.2M + others), EVENT_WRITE 0x07 375k / 0x10 231k / 0x2c 297k / 0x2e 373k,
  WAIT_REG_MEM ~590k, DMA_DATA ~124k. Full table in E22-input-route/X1/emulator-stdout.log.

### 2026-09-27 14:25-15:40 (user playtest feedback: stutter, level-change crash, A/V sync)
- User played X2 (7f9ac7c, fresh cwd = cold pipeline cache): no crash at the first transition, then a crash
  after a later level change: `Not implemented (result != eSuccess) streamBuffer.cpp:99` = vmaCreateBuffer with
  WITHIN_BUDGET failed (VRAM over budget while the previous level's resources wait for the collectors).
  26c6f72: retry past the budget; images fall back to system memory; heap budgets print with printf.
- Upstream issues checked: #745 (ASTRO BOT level-load crash, guest null deref in RoomLoad_ATQT; matches the
  PR #838 file-read-into-protected-memory failure we cherry-picked), #718 (record/replay shader precompile),
  #822/#841/#826 (audio pacing/serialization), #853 (black textures), #544/#859 status threads.
- STUTTER ROOT CAUSE (KYTY_COMPILE_LOG, 0f1626c): cold session = 232 driver pipelines, 75 s total on the CP thread
  (graphics median 110 ms, max 3.9 s; compute max 3.4 s). Translated shaders are 90-180k SPIR-V words (EXEC
  predication: Select/Bitcast/Phi dominate; tools/spv_hist.py). Guest translation 3.7 s + SPIR-V validation 3.5 s
  (--shader-validation true in the user's launcher config) per session. Warm driver cache: graphics p50 0.21 ms,
  compute all hits (X5). The cache was only saved on clean exit.
- 2801bc4 async pipelines: PrepareGraphicsPipeline (layouts, CP thread) + FinishGraphicsPipeline (driver call);
  probe with FAIL_ON_PIPELINE_COMPILE_REQUIRED (pipelineCreationCacheControl enabled), miss -> 3 below-normal
  workers, draws skipped until ready (renderDraw after AcquireRenderTargets); validation on workers; driver cache
  saved in the background once a minute. Compute stays synchronous. KYTY_ASYNC_PIPELINES=0 reverts.
  X6 (cold): boot->desert 104 s (vs 197 s), gameplay hitches max ~216 ms (shader translation), long stalls only in
  loads (sync compute pipelines, 4.8 s at the title). Worker compile total 94.6 s over 182 pipelines.
- A/V: 8b4bd35 adds guest_submit_qpc to the frame log (9th column; frame_log.py takes the first 8).
  X7: guest submit -> present end p50 55 ms (37 ms CP + 19 ms), audio host queue 10-47 ms: sound ~50 ms ahead.
  5518da9: common/avSync.h; audio target = video latency + frame - 15 ms, 40-150 ms (KYTY_AUDIO_LATENCY_MS fixes,
  KYTY_AV_LOG=1 prints). X8: queue 50-100 ms following the target.
- Harness: tools/drive.py (interactive launch/keys/shot/close, DRIVE_TEMPLATE), tools/route_desert.sh (J at title,
  RETURN/DOWN/J after the tunnel -> desert gameplay at ~100 s), tools/hitches.py (frame hitches + compile time).
- Deployed 5518da9 to Optimized Build (prev = 9ae60ef).

### 2026-09-27 15:40-17:45 (texture corruption, VRAM exhaustion, CPU profiling)
- User report (E32 window, f63ad5d): desert sand flat yellow, rocks black. Cause: async pipelines (2801bc4) skip
  draws while compiling; ASTRO BOT renders some textures once (terrain materials), so a skipped draw leaves them
  wrong for the session. 23c6adc: async is opt-in (KYTY_ASYNC_PIPELINES=1); validation-on-workers and the periodic
  driver-cache save stay. E33+ desert screenshots correct.
- VRAM: J-walking from the desert save reaches Sky Garden (#745's level); the load allocates ~2.6 GB in <30 s.
  a531315: texture GC starvation (unfreeable depth/GPU-written tiled images at the LRU head filled every pass's
  candidate window). f63ad5d/93c4666: emergency collection over the budget (images unused 5 s, GPU-written tiled
  unused 10 s, down to 1 GB below the budget; buffer GC 512/pass), system-memory fallbacks, sub-allocate a BDA buffer
  that fails as a dedicated allocation. ec00b61: free stale CPU-dirty images a new texture replaces (guard: unused
  for NumFramesBeforeRemoval ticks; 19ac6c6 without it hit 'texture requires rediscovery'). E34 passed the Sky
  Garden load; E35 (ec00b61) still ran out during it (6.46 GB, 96 KB BDA buffer refused everywhere); E36 (93c4666)
  stopped by the user's request before the load. Deployed ec00b61 to Optimized Build.
- CPU profile tools: KYTY_ALLOC_SAMPLER=1 (a22d546; tools/symbolize_allocs.py; resolve with the lld map
  _Build/clangcl-release/kyty_emulator_clang_lld_link.map, RVAs; the release PDB had no line tables),
  tools/export_sym.py (system DLL samples -> nearest export), tools/cp_latency.py (frame log + busiest thread).
- GPU thread allocations: 58k/s. 9bcf587: FindImagesInRegion results inline (128), UniqueFunction SBO (48 B),
  Image::Transit reuses its barrier vector, SynchronizeBuffer reuses its copy list, TouchImage once per tick.
- f7bbfcc: SrtWalker decoded-node evaluator for extracted plans (KYTY_SRT_VERIFY: 1.97M materializations,
  0 mismatches over boot/title/desert).
- E37 A/B desert (93c4666 vs f7bbfcc, 2+2): Thread_Gpu 90.2/91.8% -> 87.3/89.7% of a core; frame p50 32.7-33 ms
  in all (30 FPS vsync step); submit->CP flip p50 ~30 -> ~29.5 ms. The GPU thread still needs ~30 ms per frame.
- Next: release builds get -gline-tables-only (build script) for exact profiles; texture desc cache in
  ResolveTexture (uncommitted when this was written).

## 2026-09-27 wrap-up
- Branch local/astro-perf-up HEAD e02f016 (origin/main 421684e merged; pre-merge backup local/astro-perf-pre-merge).
- Optimized Build = cad952f (shader record/replay at boot, merge). e02f016 (5e428ba color-clear cache + condition
  roots) is staged, not deployed: X19 still shows MaterializeColorClear ~6 ms/frame; A/B it before deploying.
- X19 (e02f016, heavier scene, 350 frames): ResolveRenderColorTarget 7.0 ms incl, MaterializeColorClear 6.1 (7.8k
  calls), flat SRT 3.3, active sources 2.8, descriptors 2.1, PrepareBindings 3.2 self, Submit 1.5 (36/frame),
  CpOpReleaseMem 4.7 incl; GPU busy 20.8 ms/frame.
- Work paused at the user's request. Full plan (done / open issues / cheap + deep fixes / VRAM / level loads /
  research / upscaling milestone / first steps): MASTER-PLAN.md.

### 2026-09-28 01:10-01:50 (plan steps 1-3)
- Step 1 (A/B cad952f vs e02f016): decided from the X18 (cad952f) / X19 (e02f016) traces instead of a new batch
  (E38 A1 was contaminated: 170 MB free RAM, 12 FPS; E38 stopped). e02f016's targets did not move:
  MaterializeColorClear self 0.06 us/call in both, active sources 2.52 -> 2.73 us/call. Reverted both (d6511aa, fb12064).
- The "MaterializeColorClear ~6 ms" was one BufferCache::ReadMemory drain per frame (3.9-5.4 ms, zone "drain for CPU
  read of GPU data") in the CPU fallback, for a target whose metadata no code can decode. 0f58928 skips the readback
  when no code decodes (behavior-preserving). X20 (desert, drive.py): no drains left; mean 38 FPS, p50 30.9 ms,
  p99 36.6 ms (was ~30 FPS); GPU-thread wait samples ~4.5% (was 15-22%). Visuals correct.
- ASTRO BOT registers 2 x 3840x2160 display buffers although VideoOutGetOutputStatus reports resolution 1 (the
  "1080p" code, ATTRIBUTE3 bit 2 set, window 1280x720). Launcher ini (C:\ProgramData\Kyty\Kyty.ini) window = 1280x720.
- RAM: emulator ~6.8 GB WS / 6.2 GB private in the desert; with other apps open the PC had 170 MB free.

### 2026-09-28 02:00-03:00 (VRAM crash, async pipelines, resolution, PC freeze)
- e9856e3: image allocation failure no longer exits (ReclaimForAllocation frees idle images, waits for the GPU,
  retries); emergency GC 768 MiB before the budget; smart async pipelines (default on): a draw is deferred only
  when all its targets were drawn in the last 2 guest frames, else it waits for the compile. X22 cold desert:
  124 pipelines (79 s of driver compiles) moved off the frame; 107 still compiled in place (38 s).
- KYTY_PIPELINE_NOOPT test (X21): disabling driver optimization made compiles 15x SLOWER (42 s vs 2.8 s for the
  first 168) and hit a device loss -> removed.
- ASTRO BOT ignores VideoOutGetOutputStatus.resolution (values 0-4 all give 3840x2160 display buffers, X23).
  Render-target sizes (KYTY_GPU_STATS): 3840x2160 post/output passes plus DRS-scaled passes 2432x1368 / 3328x1872.
- GPU timestamps (EOP data_sel 3, COPY_DATA ref clock) are the CPU clock at recording time: the game's DRS sees
  the emulator's GPU-thread time, not real GPU time. KYTY_GPU_TIME_SCALE experiment (X24) tests DRS control.
- E40 (e9856e3, Sky Garden): VRAM usage == budget (6887 MiB), textures 4750 MiB / 1205 images all used within
  5 s, 550 MiB device-local buffers spilled to RAM, 3.9 FPS, free RAM 173 MiB -> the PC hard-froze at 02:40
  (Kernel-Power 41, no bugcheck). WDDM paging from over-commit + RAM exhaustion.
- Safety: tools/ram_watchdog.py kills the emulator when free RAM < 900 MiB for 2 s (run it during every test).
- WIP (uncommitted, build wip-prewarm1): pipeline prewarm (PipelinePrewarm: records SPIR-V modules + full create
  info to _PipelineCache/<title>.pipelines, replays on all cores at boot); VRAM headroom policy (collect from
  budget - max(896 MiB, budget/8), critical at budget - 384 MiB or free RAM < 1.5 GiB: 1 s idle instead of 5 s);
  texture age/format stats in KYTY_MEMORY_STATS.

### 2026-09-28 03:00-04:30 (resolution control, prewarm verified, hitch/upload logs, wrap-up)
- Prewarm verified (X28, clean build 9b366c4): 251 shaders in 4.8 s, 238 pipelines (227 cached, 11 compiled) in
  3.8 s at boot; during play 253 shader lookups and 234 pipeline lookups at ~0 ms, none > 100 ms.
- Dirty builds disable the driver pipeline cache -> all earlier WIP tests compiled cold. Commit before building.
- DRS steering via timestamp scale (X24): x3 -> 1920x1080 main passes, 37 FPS; x0.3 -> 3840x2160, 22.6 FPS.
  Steps are 1920/2432/3328/3840; chasing 2560 oscillated (X28, p99 133 ms) -> f064be6 holds a step in a band.
- RAM (X25/X26): emulator host ~0.9 GB; guest 6 GB at title -> 9.4 GB after 10 min (game's pool at 0xfc00000000,
  ~2 MB/s). Not an emulator leak. User's other apps ~8 GB.
- X30/X31/X33 (hitch + upload logs, 1080p mode): render targets re-uploaded from GPU-written buffers every frame,
  15-23 GB per 10 s; all marked by shader storage-buffer writes (fills/copies 0), writes cover ~27% of the marked
  image bytes; DRS size variants of one target at overlapping addresses. Travel: sustained 60-90 ms stretches
  with gpu-wait 25-33 + other 25-35 ms.
- 55ebe6d had a RAM-low eviction trigger that evicted in-use textures (8.5 GB re-uploaded in 10 s, 2 FPS) ->
  removed in 4463b70.
- Safety incident: cleanup used taskkill /IM kyty_emulator.exe and the watchdog killed by name -> could close the
  user's own session. ram_watchdog.py now kills only the drive-state pid; never kill by image name.
- Deployed 4463b70 to Optimized Build (backup kyty_emulator.cad952f.exe), emulator-settings.ini render_resolution=1080p.
- MASTER-PLAN.md rewritten (stutter-first: S1 GPL fast-link, S2 parallel translation, S3 shader/pipeline packs,
  S4 render-target round trips, S5 frame cap, V1-V3 VRAM, C1 GPU-thread CPU).

### 2026-09-28 04:35-05:30 (session 3: DEVELOPMENT-PLAN.md adopted; Phase A; dune slowdown found)
- Governing plan is now `DEVELOPMENT-PLAN.md` (user-supplied): correctness first, stable 40 FPS, Demon's Souls
  parallel track. MASTER-PLAN.md stays the status/continuation document.
- 751bd7c (Phase A): texture eviction under VRAM pressure writes GPU-written images back (tiled ones through the
  tiler; <= 32 MiB each, bounded per collection) or keeps them - no more idle-time discards; async draw skipping
  is opt-in again (KYTY_ASYNC_PIPELINES=1); resolution steering gated to the ASTRO BOT profile (PPSA21564
  01.007.000); prewarm threads <= 4 (physical cores - 1); hitch "other" never negative.
- Tools: owned_process.py (pid + creation time + exe path); ram_watchdog.py guards only that identity, reads it
  once, limit 1536 MiB free RAM or commit, exits with the process; drive.py records the identity, `kill`
  command. frame_window.py (stats between frame ids).
- Demon's Souls A5: the amdvlk64 S2 crash was already fixed by 51c13d5 (3D tiler with constant element size);
  all DS fixes (51c13d5, 23a0a3e, 2d0ffce, bb021fa, d1b9d87) are in HEAD. Next DS step per its NOTES: fresh cwd
  (keep cache, drop saves), then the job-worker spin.
- Driver (vulkaninfo, AMD 2.0.353): VK_EXT_graphics_pipeline_library with fast linking, VK_EXT_shader_object,
  VK_KHR_pipeline_binary all present.
- E39 compile log: 223 slow pipelines (124 s) use 185 distinct VS and 103 PS (PS reused ~2x, VS mostly unique);
  15 pipelines take 1.6-3.1 s each. GPL alone would split compile per stage, not remove it.
- X35 (751bd7c+stats, desert travel, 1080p): 30.5 FPS avg, p99 66 ms, two 1.9-2.1 s pipeline stalls (first-seen
  pipelines now compile in place). Host image memory = guest size (+2%): no hidden VRAM overhead in the desert.
- X36 (upload attribution): 0x53aa00000 2432x1368 RGBA16F re-uploaded every frame "for storage" (bound as a
  storage image) with buffer writes covering 17%; 0x53ad00000 1920x1080 for rt (19%); 0x53b9f0000 RG8 for rt
  (100%). Upload CPU time is only ~2 ms in slow frames.
- X36-X39 dune: after travel round 3 (Astro sliding on a dune, sand trails) frames go to 70-80 ms for as long
  as he stays: draws per frame 550 -> 1450, render passes per frame 85 -> 1200 (17k/s), image barrier batches
  2500/frame. X39 (35eda2e break stats): 95% of render-pass ends come from Image::Transit (image.cpp:243):
  images that are both the draw's attachment and sampled alternate between "attachment" and "attachment +
  shader read" access, one barrier each way, every draw (soft particles sampling the depth they test against).
- af83bbf: read-only depth/stencil attachment aspects are not stored (STORE_OP_NONE) and declared read-only;
  Image::Transit skips barriers for reads already made visible since the last write (same layout, accesses and
  stages covered; state keeps every reading stage for later WAR). X40 (af83bbf): desert section before the dune
  46 FPS, render passes 6.0k/s -> 1.75k/s, Transit breaks gone from the top list. The dune part was not reached:
  the watchdog stopped the test at 905 MiB free RAM (user's apps; free RAM before launch 8.7 GB).
- Note: a PowerShell here-string with double quotes broke a `git commit -m`, and the build then ran on dirty
  source (builds/35eda2e-dirty-rarfix-clangcl). Commit via Bash `git commit -F file`.

### 2026-09-28 05:30-05:55 (pipebench, frame cap, partial uploads; tests blocked by RAM)
- tools/pipebench (tools/pipebench/build.cmd): replays .pipelines records on the driver with a per-run
  OpSourceExtension tag (no driver-cache hits). Results (AMD 2.0.353, RX 6650 XT):
  - largest pipelines (261k words): full compile 2.4-2.7 s; spirv-opt performance passes 2.1-2.4 s CPU for
    -10% compile; GPL: pre-raster and fragment libraries ~1.5-1.8 s each (parallel wall ~1.7 s) but the
    "fast" link takes 2.2-2.4 s (with or without RETAIN_LINK_TIME_OPTIMIZATION) -> slower than one full compile.
  - mid-size (124-127k words): full ~1.0 s; GPL libraries ~0.6 s wall + link ~0.7 s = 1.3 s.
  -> GPL fast link is not fast on this driver: B3/S1 rejected for AMD Windows (re-test on driver updates).
    spirv-opt rejected (cost > saving).
- Display: desktop was 1920x1080 @ 50 Hz (monitor supports up to 190 Hz); the user switched to 190 Hz and
  wants no frame cap. 6c969f7 adds emulator-settings.ini frame_cap=off|30|20 (opt-in, not set).
- Launcher settings have vulkan_validation_enabled=true (no Khronos layer installed -> no effect) and
  shader_validation_enabled=true (SPIR-V validation of each new shader on compile workers; recommend off).
- eed41af: memory stats add PRT-tiled bytes and the largest-mip share (for the Sky Garden VRAM question).
- ce95f9f: partial render-target uploads (block rows covering the buffer-written range, 128-row groups,
  RenderTarget64KB single-level images not CPU-written since their last upload); KYTY_PARTIAL_UPLOADS=0 off.
- tools/stack_sample.py + stack_report.py: dbghelp StackWalk64 stack sampling of one thread, inclusive/self
  time per function (untested: the run was stopped before sampling).
- X40/X41: the watchdog stopped both runs during the intro route (905 / 775 MiB free; the route needs
  ~8.6 GB, available was 8.7 / 9.4 GB). Unverified so far: af83bbf (dune), ce95f9f (partial uploads).
  X41 before the stop: render passes 1.4-1.6k/s (vs 6k/s before af83bbf), slow frames mostly gpu-wait.

### 2026-09-28 05:55-06:15 (codegen measurements; compile notice; X42 stopped by RAM)
- 6ea9551: window title shows "compiling shaders for a new area (N), only the first time" from the 3rd
  non-cached pipeline compile in a row on the GPU thread, cleared 2 s after the last.
- Codegen (pipebench --dis / --asm, tools/flatten_guarded_loads.py) on the desert's largest pixel shader
  (152k words, 37k instructions): bounds-guarded buffer loads are if-diamonds (262 of 342 conditional
  branches); making them branchless (load + select, valid because robustBufferAccess is always on) cut its
  compile 3.0 s -> 2.75 s (~9%). Indexed register reads (GCN movrel) are 65-way IEqual+Select chains:
  53 chains, ~6.7k instructions (18%). Bitcasts 6.4k are ~free.
  -> Local codegen fixes might give 20-30% on first-visit compiles, not more. Any codegen change also
     invalidates the user's driver cache (every visited area compiles once again), so the branchless-load
     change was reverted until it can ship with bigger codegen gains and a prewarm that survives codegen
     changes (pipeline records keyed by shader recipe, not SPIR-V hash).
  -> The only large first-visit lever is compiling several pipelines at once, which needs deferred
     Vulkan recording or PM4 look-ahead (architecture work).
- X42 (6ea9551): stopped by the watchdog at 1340 MiB free during the intro route (10.2 GB available at
  launch): the route peaks at ~8.9 GB. Launch only with >= ~10.6 GB available.

### 2026-09-28 06:15-06:40 (X43 verification, deployment of 78256f4)
- X43 (78256f4 = all session-3 changes, desert + 3 travel rounds + dune, 11.0 GB available): no errors,
  visuals correct (sand, trails, dust, lighting). Dune: render passes 17k/s -> 1.2-1.9k/s, image barrier
  batches 36k/s -> 3.4-4.4k/s, 10 s windows 184-224 frames (18-22 FPS) vs 133-187 (13-19) in X39.
  Normal travel windows unchanged (326-456 frames/10 s in both). Partial uploads skipped 5.5 GB of
  ~9.8 GB per 10 s.
- Dune stack sample (stack_sample.py, 300 samples): MaterializeResources (SRT walk) 40% of the GPU
  thread, of it FindActiveSources 18%, RefreshFlatBuffer 14%; DrawIndexAuto (particles) 59% of samples;
  PrepareGraphicsBindings 12%, amdvlk 10%, PrepareProgram 6%. ntdll/VCRUNTIME leaf time ~16% spread over
  mutexes, per-draw shader-memory reads and backing-store lookups.
- Deployed 78256f4 to Optimized Build (backup kyty_emulator.4463b70.exe/.pdb, build-manifest.4463b70.txt);
  emulator-settings.ini documents frame_cap=off; README rewritten.

### 2026-09-28 06:40-07:00 (per-draw CPU cost; test launch declined by the user)
- The user declined a test launch (X44); no emulator was running. Continued with code only.
- Dune profile inside MaterializeResources: self time EvaluateNode 16%, NodeArg 11%, element
  construction 10% (memo read capture push_backs, pipelineCache.cpp CapturingStrictRead/ObserveDirectRead),
  EvaluateIndex 9%; FindActiveSources 44% and RefreshFlatBuffer 34% of the walk inclusive. Two walkers
  (clean/strict and observed) evaluate overlapping values: merging them = the compiled-SRT redesign (D2).
- 360189d: memo validation compares user-data registers before re-reading guest memory; KYTY_SRT_MEMO_STATS
  reports why misses happen (first walk / memory changed / which user-data register changed).
- d6ca050: a shader whose memo missed 16 times in a row walks its next 256 draws without memo capture
  or validation (pure cache; results identical).
- 7fd5120: SRT walker evaluates nodes by reference (table sized once to every evaluation index).
- Staged builds\7fd5120-clangcl (not deployed; deployed is 78256f4). Next: one dune run with
  KYTY_SRT_MEMO_STATS=1 + stack_sample to measure the per-draw savings, when the user allows a test.

### 2026-09-28 07:00-17:30 (session 4: OPUS-AUTONOMOUS-MASTER-PLAN.md is the governing plan)
- 7fd5120 (walker nodes by reference) broke rendering (user: black title/menu background): lazy
  `EvaluationIndex` growth reallocates `srt_nodes` mid-walk. Reverted in 6148de9; X45 rendering OK.
  X45 also showed heavy horizontal striping in a dark intro scene: NOT yet attributed (compare with
  KYTY_PARTIAL_UPLOADS=0 / an older build).
- X40-X42, X44, X46 stopped by the watchdog: the user's apps + the ~10.2 GB test working set exceed RAM.
  Watchdog floor lowered to 1024 MiB on evidence; the user closed apps.
- 0a820d3 draw-size stats (X48): the dune issues ~14k direct draws/s of <= 6 vertices x instances
  (sand trails / particles); the PS walk is 72% of walk time.
- b1fdf8a delta stats (X49): between consecutive walks of the same shader 85-87% of descriptors and
  97% of flat SRT words are unchanged, but 0-10% of walks are identical (SRT pointers move per draw).
  d6ca050 memo bypass: dune still ~17-22 FPS windows, walk share 40% -> 36%.
- adb9a0c incremental walks (keep values whose user-data / memory buckets did not change):
  X50 KYTY_SRT_INCREMENTAL_VERIFY=1: 3.08 M walks compared with full walks, 0 mismatches.
  X51 (on) vs X52 (KYTY_SRT_INCREMENTAL=0), same route + travel 4 + 300-sample stack on the dune:
  MaterializeResources 45% vs 34% of the GPU thread (ReadAgain alone 6.7%); dune windows 10-19 FPS
  in both (the bookkeeping runs even when off). Reverted in a4d09c0. Steam was using ~0.75 core
  during X51/X52 (confounder for absolute FPS, not for the A/B).
- Demon's Souls S11 (adb9a0c, fresh cwd: pipeline cache kept, no saves; validation off via
  drive.py DRIVE_GAME/DRIVE_PATCH=none/DRIVE_NO_VALIDATION=1): boots past the S8-S10 early exit,
  the opening cinematic renders correctly at ~31 FPS (S7 on bb021fa: 1.7 FPS), reaches character
  creation with correct visuals at 2-19 FPS. Every frame re-uploads ~19 textures / 107 MiB
  (hitch log "new textures"); color targets 3840x2160 (~1.7k binds/s) and 2560x1440; 13 job workers
  busy-spin (3 at 95%, 8 at ~50%). Working set grew 2 -> 9.1 GB in 7 min; at 440 s free RAM fell
  1.8 GB -> 1 MiB within 5 s and the watchdog stopped it (source of the spike unknown; the watchdog
  now logs the test's working set / private bytes the moment RAM goes low).

### 2026-09-28 (isolated native-1080p negotiation research)
- Created `NATIVE-1080P-ISOLATED` with its own source worktree, copied save/cache, launcher state,
  and build output. Active `kyty ps5-src` remains at `ef996b5` with the same two pre-existing edits;
  no active or deployed build was touched.
- Extended the opt-in VideoOut trace to record `VideoOutSubmitChangeBufferAttribute2` old/new
  extents as well as status and registration. Isolated Release build succeeded; SHA-256
  `85dff0ca6dc45f5d578e75425391a742b164a97f6dc5ec14360e57a0a7a095c2`; candidate is not deployed.
- Existing X23-res1/X47 evidence still shows a 1080p output hint with 3840x2160 registered buffers.
  The presenter copies the guest source extent, then the host swapchain scales to its drawable.
  ASTRO's legacy DRS setting does not prove native 1080p; X47 observed 1920/2432/3328/3840 scene
  targets and a clock scale reaching 8.0.
- X47 dune samples point to CPU command preparation (98.7% of GPU-thread samples in PM4, 63.0%
  in `DrawAuto`, 43.7% in `GetGraphicsPrograms`); actual GPU busy percentage is unmeasured. The
  separately selected final 20-second window is 21.7 FPS with p99 70.8 ms and a 13.3 FPS 1% low.
- No ASTRO run was launched: available RAM remained below the ~10.6 GiB prelaunch gate. Demon's
  Souls S16 also shows a severe pressure case (end-selected 20 s: 6.9 FPS, p99 2499 ms); the
  PID-owned watchdog stopped it at 315 MiB free while the test working set exceeded 11 GiB.
- A clean isolated CMake setup fetched its pinned FFmpeg prebuilt into the isolated build cache;
  it is cached there for this candidate. No downloads or modifications occurred in the active
  source, `Fixed Build`, or `Optimized Build` after that.

### 2026-09-28 (isolated resolution/presenter trace continuation)
- Audited the complete static output chain: host screen config feeds WindowInit and VideoOutInit; VideoOut status is a hint derived from host context plus ATTRIBUTE3; guest display-buffer extent/pitch/tiling/metadata remain supplied and validated by the guest.
- Added bounded, opt-in presenter extent logging to the isolated source: guest logical extent, resolved backing image, prepared presenter frame, and host drawable. Distinct extent tuples are logged once. This diagnostic does not modify render dimensions or resources.
- Built and staged NATIVE-1080P-ISOLATED/builds/resolution-trace-presenter-ef996b5 (SHA-256 f614e82a895627865e2abb01040a1341e95373a4cdee2ff063951c4ff14177a5); the isolated launch manifest now points to it. It is not deployed.
- The existing X47 dune sample is CPU-command-preparation heavy; actual 3D GPU utilisation remains unmeasured. No runtime test was launched in this continuation because available RAM remained below the established game-run gate. The next action is the single short boot trace when system headroom is safe.
- Cross-emulator review of shadPS4 PR #3194 found a useful host-window/internal-screen separation pattern, but its discussion warns games may treat reported resolution as a capability heuristic. Kyty ASTRO already ignored the 1080p hint for its registered 4K display buffers, so this is not the native-resolution fix.

### 2026-09-28 (isolated ASTRO 1080p runtime negotiation)
- Boot trace with render_resolution=1080p reported the 1080p output hint but registered two 3840x2160 guest display buffers; the prepared presenter image was 3840x2160 and the host drawable was 1280x720. This is a true runtime confirmation that the current setting does not yield native 1080p output.
- The legacy DRS feedback reached scale 8.00 while selected screen-sized targets rose to 3840. A reversed-direction isolated candidate lowered the scale to 0.58-0.69, but selected target widths still included 3840 within the short observation. Cause and convergence remain unproven.
- Both tests were boot-only, with replay/prewarm/validation off, and ran under a 3 GiB exact-PID watchdog. Working set exceeded 5.6 GiB within 30 seconds. They were closed cleanly before the watchdog threshold; free RAM recovered to about 9.6 GiB.
- Current isolated candidate: resolution-control-reversed-ef996b5, SHA-256 de1281d8043186cc72c3c124aa170cf22eb94aba4b515f537d0d60528e93545c. No candidate deployed; active source and builds remain untouched.
- Existing X47 dune samples remain 18.8-21.7 FPS with 70-72 ms p99 and a CPU-heavy PM4/per-draw preparation stack; physical GPU busy is unknown. Require about 10.6 GiB available before the dune route.

### 2026-09-28 — isolated target histogram and clock safety
- `resolution-width-attribution-ef996b5` logged 8-frame color-target bind histograms during boot. The winner moved between 1920, 2432, 3328 and 3840; the legacy controller changed its guest-clock scale from 1.00 to 0.48 while 3840-pixel targets still won some windows. This sensor does not identify the primary scene target.
- The exact-PID watchdog terminated that boot test when free RAM crossed below 3 GiB; the trigger sample was 2.9 GiB. It targeted only the recorded isolated test PID; RAM recovered to about 9.7 GiB. The title bar's 45 FPS observation was in the opening sequence, not the dune.
- Isolated commit `569d146` disables legacy guest-clock steering by default and keeps it behind `KYTY_LEGACY_DRS_STEERING=1`. Clean candidate `NATIVE-1080P-ISOLATED\\builds\\resolution-observe-no-steer-569d146`, SHA-256 `19EDD8FD2A638F203F437C40B2AD35002D68B53FC75EE200A8E40DD715E41F20`. An 18-second boot closed cleanly via WM_CLOSE; it remained at shader loading and still registered two 3840x2160 guest display buffers despite a 1080p mode hint.
- Dune testing deferred: free RAM was 9.69 GiB at launch, below the 10.6 GiB gate. No new gameplay FPS or GPU-utilization result. `NATIVE-1080P-PLAN.md` and isolated `RESEARCH-NOTES.md` record the next steps; active source and player builds remain untouched.

### 2026-09-28 — target identity trace package
- Isolated commit `fc569fa` now records top color-target identities (address/range, extent, format, mip/layer, samples, tiling) per 8-frame window under `KYTY_RESOLUTION_TRACE=1`, using a bounded 128-entry table and 900-window output cap. No draw/render behavior or reference-clock behavior changed.
- One-worker Release build passed. Clean staged research candidate `NATIVE-1080P-ISOLATED\\builds\\resolution-target-identity-fc569fa`, SHA-256 `64B6098E2B63BA454895E811C900C24492D0906600078CC782441F9397185DA6`; not deployed. Runtime validation remains pending.
- Dune remains deferred: available RAM is ~9.7 GiB versus the 10.6 GiB gate. The test patch disables GI/lighting and is not suitable for a full-fidelity visual claim.

### 2026-09-28 — isolated shader profiling and dune telemetry
- In NATIVE-1080P-ISOLATED only, committed diagnostic scopes at GetDeclaredShaderHash and GetShaderParams (4976070); clean single-worker Release build passed, candidate SHA-256 540B5251881E437EEBFD69B7187F5CE51B945729F111E7C2CAE18FBB76130395. No runtime gameplay validation.
- Added exact-PID GPU/CPU/RAM sampler under the isolated tools folder. PowerShell parser and unrelated-PID guard passed. Windows GPU counters enumerate; per-LUID mapping to RX 6650 XT remains to be confirmed from a live owned process.
- Source review corrected the timing model: Tracy has GPU query zones under --profile, while guest COPY_DATA/EVENT_WRITE reference-clock values are synthesized from host TSC. X47 had no Tracy trace, so physical dune GPU busy remains unknown.
- Free RAM 9.67 GiB was still under the 10.6 GiB gameplay-route gate. No game launched. Native 1080p remains unconfirmed: previous runtime registered two 3840x2160 guest buffers despite the 1080p mode hint.
- Next decisive test: one short windowed, uncapped dune capture when the real RAM gate is met; sampler-only for performance and a separate brief Tracy interval for CPU/GPU attribution.

### 2026-09-28 — compute-path attribution candidate
- Read Prosper issue #1732 as a cross-emulator lead: its author measured a compute-path throughput difference for ASTRO BOT but states that the mechanism is unknown. No compute work was disabled or skipped in Kyty.
- Isolated Kyty commit 9bfa43f adds only profiler scopes around DispatchDirect, DispatchIndirect, and GetComputeProgram. Clean single-worker Release build passed; candidate SHA-256 59D1DF8E69C748020BF902C932EEFF16376A6C2EAF68A11C9A299EA7D5C5D7FE. Not deployed and not runtime-validated.
- The exact-PID telemetry sampler and existing Tracy guest-dispatch GPU zones are ready for one bounded attribution run. Current free RAM is 9.26 GiB, below the 10.6 GiB route gate; no game process is running.
- Native 1080p remains unresolved; a 1080p mode hint still coexisted with two 3840x2160 guest display buffers. Next run remains gated on available RAM and must correlate CPU compute preparation with GPU dispatch time and the final output path.

### 2026-09-28 — scanout range trace and bounded boot check
- Ran one short, isolated, windowed boot check with shader replay, pipeline prewarm and validation disabled. The game remained in the opening cave cinematic and did not reach the menu or dune. A brief titlebar observation near 45 FPS is not a gameplay benchmark; no GPU-utilisation result was collected.
- Runtime again returned a 1080p output hint under the 1280x720 host context while ASTRO registered two 3840x2160 guest display buffers. Target-identity windows included 1920x1080, 2432x1368, 3328x1872 and 3840x2160 resources. These identities are not yet classified as scene, post-process, history, UI or scanout.
- Offline parsing of the top-six-only identity rows found 3840x2160 in 135 windows/8,671 reported binds; 3328x1872 in 50/2,628; 2432x1368 in 16/6,538; and 1920x1080 in 32/2,407. Counts are lower bounds. Recurring addresses `0x0520440000` (3840) and `0x05168c0000` (2432) are output-chain candidates pending exact flip-range and copy/resolve correlation.
- Format 58 is Vulkan A2R10G10B10 UNORM and matches the current VideoOut format mapping. The target trace shows two adjacent, recurring 3840x2160 format-58 ranges at `0x0507410000` and `0x05093f0000`, each `0x1fe0000` bytes (~31.9 MiB), strongly suggesting the double-buffered scanout pair. Await exact registration/flip range match. Their combined ~63.8 MiB is far too small to explain multi-GB VRAM exhaustion by itself. Format 97 is R16G16B16A16_SFLOAT (8 bytes/pixel) and appears at the DRS-like sizes; those full-resolution scene/HDR candidates deserve priority once their roles are mapped.
- Presenter input/backing/presenter extents stayed 3840x2160 while the host drawable was 1280x720; this did not confirm native 1080p rendering or host output.
- The latest trace exactly matched the two registered buffers and first 32 flip ranges: buffer 0 `0x0507410000` and buffer 1 `0x05093f0000`, both `0x1fe0000` bytes; 16 flips each. This confirms the double-buffered 4K scanout pair, but does not classify scene/HDR targets.
- An earlier boot's watchdog observed working set peaking at 6254 MiB and terminated only test PID 18596 after available RAM reached 2045 MiB. In the latest scanout run, PID 2580 started with 9046 MiB available and was gracefully closed at 3634 MiB after about 37 seconds; RAM recovered to 8.92 GiB. The first background watchdog invocation omitted quotes around spaced paths and created no log; corrected quoting was smoke-checked against the exited PID. No transition/dune or GPU-use test was run; the 10.6 GiB gameplay gate remains unmet.
- Hardened the isolated launch tool after that run: it now automatically starts the exact-PID watchdog with a 3072 MiB default threshold and logs into the run directory. Python syntax and process-spawn argument handling were checked against the exited test PID; safe-exit log confirmed. This was a harness-only change, no emulator rebuild or gameplay launch.
- Source audit found the isolated `render_resolution=1080p` option sets a candidate-width observer; with guest-clock steering disabled it does not change rendering. The status hint is not evidence of native 1080p.
- Added bounded, opt-in registration/flip address-range tracing in isolated `videoOut.cpp`. Single-worker Release build and `git diff --check` passed. Candidate `NATIVE-1080P-ISOLATED\builds\scanout-range-trace-9bfa43f-dirty`, SHA-256 `D03B78857EADA3B3DB240F19E971ABD32E4D3744809544C67C6B44F4A5B6CF07`; short boot trace validated address matching, with no gameplay/performance validation. It is not deployed; launcher manifest points to it.
- Next: map those ranges to the target/copy/resolve chain before changing dimensions. Wait for at least 10.6 GiB available RAM for one bounded dune attribution pass; sampler-only for performance, a separate brief Tracy profile for CPU/GPU causality. Active source and deployed builds remain untouched.

### 2026-09-28 17:30 - 2026-09-29 03:35 (session 4 continued: DS diagnosis, upstream merge, dune walk work)
- DS uploads: S13-S16 with KYTY_UPLOAD_LOG / KYTY_IMAGE_CHURN_LOG: up to ~55 GB of image uploads per 10 s at
  the cinematic. 3e6ecc8 (dirty 64ths) changed nothing: the compute writes cover the images. The 3840x2160
  RGBA16F target at 0x282e50000 was deleted by ResolveOverlap (textureCache.cpp:1076) and re-inserted 248x
  per 10 s: "safe to delete" counted submission ticks, and DS submits many per frame. 5fd3a15 keeps images
  looked up in the last two guest frames (RenderStats::g_guest_frames). Runtime check pending.
- DS VRAM (S12 gameplay): 6,984/6,984 MiB, 3,006 emergency evictions and 1.9 GB written back per 30 s.
- DS files: 164 GB / 223k loose files on S: (5400 rpm HDD); C: has 14 GB free. 0629bef: APR path lookup
  does one GetFileAttributesEx instead of three. Job workers: work-stealing scan loop (guest 0x90086e000).
- Upstream merged (17363db, 23 commits to 539c0f7): conflicts resolved (see the merge commit), new deps zstd
  and ZArchive fetched with the user's approval into _Build\deps-cache (build script updated).
- NATIVE-1080P research branch reviewed: diagnostics only (ASTRO keeps 2x 3840x2160 display buffers whatever
  mode is reported; scanout pair ~64 MB). Its steering-off default would remove the measured desert gain:
  not merged. PERF-MASTER-PLAN.md written (dune/CPU/RAM/VRAM, DS as stress test).
- Dune walk work: d3ce7f8 FindActiveSources per-plan base + decoded condition roots; 8df78aa per-walk cache of
  GPU-clean 4 KiB pages for strict reads (GpuCleanReadScope, backing map generation); a424fbb observed walker
  reuses clean-walker values (KYTY_SRT_SHARE_CLEAN=0 disables).
  Verified (X53/X54 ASTRO boot + intro, 1280x720): SRT verify 458,752 walks 0 mismatches; clean-read verify
  9.37 M cached reads 0 mismatches; title/logo render correctly. NOT measured on the dune yet: available RAM
  was 5.9-7.0 GB (Discord, Edge open); X55 intro A/B at a fixed time sampled different scenes (invalid).
- Next: dune A/B 78256f4 vs a424fbb (and a424fbb with KYTY_SRT_SHARE_CLEAN=0) when >= 10.6 GB is free;
  DS cinematic run to confirm the 4K churn is gone.

### 2026-09-29 03:36-04:20 (machine freeze during DS S17; safety changes)
- S17 (DS, build 0521196 = upstream merge + walk changes + overlap/eviction policies) ran ~4 min into loading
  (438 game frames), then the PC froze hard: Kernel-Power 41, BugcheckCode 0, no TDR/bugcheck event, last
  emulator log line ~03:39:40 (pipeline cache saved). RAM was fine (4.7 GB free, WS 4.9 GB), VRAM fine
  (2.8/7.4 GB). Unlike the 9/28 02:40 freeze (RAM at 173 MiB). S17 was the first run with several upload
  bands per image (the 4K target now kept alive: 5 of 64 bands changed) and possibly standard 64 KiB bands.
  Upstream #894 (block image storage usage) is the other new factor. Cause NOT isolated.
- df9fec0: Image::Upload/Download bounds-check every copy region (EXIT with a message instead of an
  out-of-bounds GPU copy); KYTY_PARTIAL_UPLOADS_EXT=1 now enables the extended band uploads (default back to
  ASTRO-verified render-target single band). Vulkan validation layer is not installed on this PC.
- DS testing paused until the user agrees to the crash risk; next DS run would be df9fec0 (bounds checks on,
  extensions off). ASTRO dune A/B still needs >= 10.6 GB available (9.2 GB now).

### 2026-09-29 (after the freeze) RAM measurements, second upstream merge
- X56/X57 (ASTRO boot, 75 s): working set 5.7 GB = guest-mapped 4.34 GB + host-private 0.95 GB. 6bf26df
  (flexible memory: zero only reused backing) changed nothing at boot (5.73 vs 5.71 GB): ASTRO's boot RAM is
  guest direct memory the game (or the emulator's first reads of GPU-only memory) touches.
- Second upstream merge (ce71faa + 8560b11, 13 commits to 59a1760): per-draw CPU work (37501b5, 0fbeac6,
  5e4f6af, b43bf9f, e57c37f), 6f24b03 short sleeps block instead of busy-waiting, 8e42e99 label batching.
  Kept: our pipelineCache mutexes (prewarm/async need them; the clean merge had dropped two locks, restored),
  our bounded RELEASE_MEM flush coalescing. Builds. NOT runtime-tested: at 19:04 the ASTRO game folder in
  Downloads lost its eboot.bin (the user is moving the game to S:\[SuperPSX]-ASTRO.BOT...\PPSA21564-app,
  which has eboot.bin). Tests must use DRIVE_GAME pointing at the S: copy once the move is complete.
- 25aaf07 low-memory guard: background thread, title warning below max(1 GB, stop+512 MB), clean stop below
  low_memory_stop_mib (default 400, emulator-settings.ini / KYTY_LOW_MEMORY_STOP_MIB, 0 off) for 2 s.
  X59: forced with KYTY_LOW_MEMORY_STOP_MIB=99999 -> stopped at 2 s with the message (6482 MB free). Works.
- ASTRO game move C: -> S: still incomplete at 19:25 (S: lacks sce_module/sce_sys). No ASTRO tests until done.
- 46e2a7d opt-in texture_quality=reduced (KYTY_TEXTURE_QUALITY): large sampled-only textures without their top
  mip on the GPU (ImageInfo::host_mip_skip), promoted to full quality on any exact-texel/target/storage/copy
  lookup; CopyImage/Download refuse reduced images; MEMORY_STATS reports created/promoted counts. Builds; not
  run yet (ASTRO files mid-move; DS paused after the freeze).
- State for the next session: HEAD 46e2a7d on local/merge-upstream-0929 (= local/astro-perf-up + two upstream
  merges + everything above). Staged, not deployed: 25aaf07 (guard), 46e2a7d (texture quality). Deployed is
  still 78256f4. Next: (1) ASTRO smoke with KYTY_SRT_VERIFY/KYTY_CLEAN_READ_VERIFY on 46e2a7d, game at its new
  path; (2) dune A/B 78256f4 vs 46e2a7d when >= 10.6 GB free; (3) Sky Garden with texture_quality=reduced
  (VRAM use, emergency evictions, FPS); (4) DS only with the user's OK.
- Package 4 check (code + S12/S15/S17 logs): DS indexed indirect draws never take the CPU-args path
  ("CPU reads the arguments" logged 0 times); GPU arguments are used (18k indirect draws/s in gameplay).
  DS's gpu-wait (43-64 ms/frame) is therefore not indirect-args readback; needs GPU timestamps.
- fc8a947: KYTY_GPU_STATS attributes render-pass breaks from Image::Transit ("transit OLD->NEW WxH FMT") -
  for DS's 1,239 of 1,553 passes/s ending there. Needs a DS run (user consent) or an ASTRO run to read.
- PERF-MASTER-PLAN.md section 3 rewritten around the user's 9 packages / 3 tracks; threading expectation
  corrected (walk-only parallelism <= 1.34x on the dune).
- S18 (DS, fc8a947, fresh cwd, 7.9 GB available at start; ASTRO file copy to the HDD holding ~8 GB of file
  cache): boot faster than S12 (frame 1837 at 31 s vs 1788 at 104 s: 0629bef / upstream, not isolated);
  cinematic 24-27 FPS, correct visuals. 4K target churn gone (5fd3a15 confirmed); partial uploads skip
  12.5-14.2 GB per 10 s (S13-S15: ~0.5 GB) -> real upload traffic ~30 instead of ~55 GB per 10 s.
  Render-pass breaks at the cinematic: global barriers (graphicsRun.cpp:1460) ~280/s, Image::Transit ~60/s
  (ColorAttachment->ColorAttachment 1920x540 RGBA16F: attachment access mismatch after a clear). RAM at the
  cinematic: WS 4.7 GB = guest 3.5 GB + host-private 1.07 GB. At 757 s (tutorial loading, compiles "area
  (99)") the test WS reached 9.6 GB and free RAM fell to 28 MB: watchdog stop. DS gameplay needs more RAM than
  this PC has free while the file copy runs.
- S19 (f701283, texture_ram=trim): WS 6.8 -> 10.6 GB in ~11 s at ~545 s with private bytes flat (guest pages
  made resident in a burst); trim churned 131 GB on compute-rewritten textures -> df00497 trims only
  CPU-written uploads and logs buffer uploads (only MBs per 10 s: not the burst source).
- S20 (df00497): DS reached tutorial gameplay with correct visuals; WS 8.7-9.0 GB, 2.9 GB free at 834 s.
  Gameplay 1.9-2.4 FPS during first-visit compiles; 831 passes/s, 673/s from "transit
  DepthReadOnlyStencilAttachment -> same 2560x1440 D32S8" (depth target sampled + attachment with two
  access masks). Crashed at 838 s: guest fault in a job worker (pc 0x900b3c079) then the fault handler
  re-entered 5x (PrintHostBacktrace faulted in ntdll).
- 9c52585: depth target sampled read-only asks for the combined access (targets the 673/s); unverified.
- Session wrap-up: handoff in kyty ps5-src/perf-handoff (FINDINGS, CLOUD-AGENT-MASTER-PLAN, recipe, logs,
  tools, build 9c52585) + CLAUDE.md for a cloud agent; branch local/merge-upstream-0929.
