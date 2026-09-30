# Cloud state (update at every checkpoint)

## Goal of this cloud run
1. Milestone A: green Linux build, every test counted and classified.
2. Milestone B: stability and correctness fixes with tests (fatal path, bounds, persisted-file integrity).
3. Milestones C and D: reliable shader/pipeline reuse across runs; cheaper dune draw preparation.
Behaviour changes are opt-in unless provably equivalent. No performance claims from the cloud.

## Source identity (verified 2026-09-29, cloud checkout)
- Branch `claude/eloquent-meitner-6tzgiw`, started at `9ceebc4` (= `main` = `origin/main`).
- Full performance lineage present (`9c52585`, `e49dd65`, perf-handoff docs). The uploaded REVIEW.md
  concern (unborn `floppa` branch without prewarm/replay files) does not apply to this checkout.
- Submodules: all 14 initialised (shallow). Toolchain: clang 18, lld, ninja, cmake 3.28, glslang,
  mesa-vulkan (lavapipe), libclang-rt-18-dev (ASan/UBSan).
- Build dir `_Build/linux-no-qt` (Release, launcher off). 4 cores, 15 GB RAM: build with `-j3`.

## Capability inventory (source evidence)
| Feature | Present | Where |
|---|---|---|
| Shader precompile (recorded GCN permutations replayed at boot) | yes | pipeline/shaderPrecompile.cpp, PipelineCache::StartPrecompile (`_PipelineCache/<title>.shaders`, KYTY_SHADER_PRECOMPILE=0 off) |
| Pipeline prewarm (SPIR-V + create info replayed at boot) | yes | pipeline/pipelinePrewarm.cpp (`<title>.pipelines`, KYTY_PIPELINE_PREWARM=0 off, KYTY_PREWARM_THREADS) |
| Driver pipeline cache persistence | yes | PipelineCache::InitializeDriverCache / WriteDriverCache (`<title>.bin`; signature vendor/device/driver/UUID + XXH3; temp file + rename; disabled on dirty/unknown git builds) |
| Async pipeline compiles | yes, opt-in | KYTY_ASYNC_PIPELINES=1, KYTY_PIPELINE_WORKERS (clamped 1-8, default hw/3 in 1-4) |
| SRT decoded-node evaluator, per-plan scratch, verify | yes | ir/passes/SrtWalker.cpp, ResourceMaterialization.cpp (KYTY_SRT_VERIFY, KYTY_SRT_SHARE_CLEAN) |
| GPU-clean page cache for strict reads | yes | kernel/memory.cpp GpuCleanReadScope (KYTY_CLEAN_READ_VERIFY) |
| Materialize memo | yes | pipelineCache.cpp MaterializeMemo |
| Partial render-target uploads / extended bands | yes / opt-in | textureCache.cpp (KYTY_PARTIAL_UPLOADS, KYTY_PARTIAL_UPLOADS_EXT=1) |
| Texture RAM trim | opt-in | textureCache.cpp + kernel/memory.cpp TrimGuestWorkingSet (texture_ram=trim / KYTY_TEXTURE_RAM) |
| Reduced texture quality | opt-in | ImageInfo::host_mip_skip (texture_quality=reduced / KYTY_TEXTURE_QUALITY) |
| Low-memory guard | yes | main.cpp, window.cpp (low_memory_stop_mib / KYTY_LOW_MEMORY_STOP_MIB) |
| Fault handler recursion guard | yes | runtimeLinker.cpp KytyExceptionHandler (e49dd65) |
| Depth sampled read-only access | yes | renderDraw.cpp AcquireRenderTargets (9c52585) |
| RELEASE_MEM flush coalescing | yes | CommandProcessor::BufferFlushCoalesced |

## Build notes
- Re-configuring an existing build dir can fail in the zarchive FetchContent patch step
  (non-idempotent `git apply`, 3rdparty/CMakeLists.txt). Prevent it once per build dir with
  `cmake -DFETCHCONTENT_UPDATES_DISCONNECTED=ON <build dir>` (the update and patch steps then do not
  re-run; done for `_Build/linux-no-qt` on 2026-09-30). If it fails anyway: revert with
  `git -C _Build/linux-no-qt/_deps/zarchive_source-src checkout -- .` and reconfigure. Afterwards CHECK
  the patch is applied: `git -C _Build/linux-no-qt/_deps/zarchive_source-src apply --check --reverse
  --ignore-whitespace 3rdparty/patches/zarchive-reader.patch` must succeed; if not, apply it
  (`... apply --ignore-whitespace ...`) and rebuild, or archive_file fails. CMake left unchanged on
  purpose: the owner's offline Windows deps cache is not in this checkout.
- Linux build breakages found and fixed: Tracy link (eb8539c), allocSampler throw (b2d1654),
  Windows-only config call (1d2544c), avplayer timer link and non-copyable Pipeline in tests (625aed4).

## Status at end of cloud session (2026-09-29)
- Milestone A: Linux build green; ctest 47/50 on lavapipe (kernel_file_system blocked by the
  environment; 2 upstream tests encode behaviour this fork changed on purpose, R7). GPU lane runs with
  Vulkan sync validation: no sync hazards left.
- Milestone B (stability) done in this session: fault report red-zone dump; tiler buffer range checks;
  prewarm file integrity (module hash, stale pointers, sType); low-memory guard zero reading + opt-in
  fast stop; 2D PRT tiler fixed-element builds; download staging WAW barrier; accounting underflow no
  longer exits; stencil dynamic state set once per command buffer.
- Opt-in features added: low_memory_fast_stop, KYTY_RAM_ATTRIBUTION, KYTY_WAIT_INVENTORY, trim
  cooldown (inside texture_ram=trim), explicit-LOD exclusion (inside texture_quality=reduced).
- Tests added: pipeline_prewarm_format, low_memory_guard, differential SRT walks, explicit-LOD
  tracking, GPU band parity, harness optional features; stale tests fixed (shader_cfg mesh prolog,
  non-copyable Pipeline, avplayer link).

## Status at end of cloud session (2026-09-30, HEAD on claude/eloquent-meitner-6tzgiw)
- Upstream KytyPS5 merged to 05057c9 (5578667): 28/30 affected tests pass, 2 known R7 failures.
- Dune per-draw CPU (main focus): SRT evaluator fast paths (80ba8b2) cut the resource walk from
  80.5k to 45.9k instructions per walk on the dune-shaped benchmark (-43%, identical results to the
  IR interpreter); memo bypass doubles while a shader keeps missing (d6b8b16). Neither is measured
  in the game yet (recipes 22, 25).
- Tools: srt_walk_bench (walk benchmark + differential, 3639b4d), dune_draw_bench (whole draw path
  through DrawAuto, 1ffa08c). perf works in this container (linux-tools-6.8.0-142, installed per
  session: `apt-get install linux-tools-common linux-tools-6.8.0-142-generic`).
- Stability: internal blit re-sets the dynamic stencil state (8680195).
- Compile stutter: audited (precompile + prewarm + persistent driver cache already make second
  visits compile-free; draws are never skipped by default); driver-cache save now logs its duration
  (0753b0d, recipe 24).

## Next (in order)
1. Owner: build 1ffa08c (or later) with clang-cl, run recipe 22 (KYTY_SRT_VERIFY + dune A/B with
   tools/ab_dune.sh, which also samples the GPU thread) and 21, 24, 25; then 10-20.
2. With the new dune profile: next per-draw candidates from dune_draw_bench (program lookup key
   building/hash per draw, render-target resolution per draw, texture lookups).
3. If recipe 24 shows long saves during play: save the driver cache less often (after N new
   pipelines and at exit).
4. If recipe 20 shows zero-length sleeps from DS job workers: opt-in yield on zero sleeps.
5. If recipe 19 shows emulator-caused residency: avoid first reads of GPU-only memory where a full
   overwrite is proven (plan W5c).
6. Update the two upstream texture-cache tests for the view-format list (R7) with the next upstream
   merge.
