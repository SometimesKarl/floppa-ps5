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
  (non-idempotent `git apply`, 3rdparty/CMakeLists.txt). If it fails: revert with
  `git -C _Build/linux-no-qt/_deps/zarchive_source-src checkout -- .` and reconfigure. Afterwards CHECK
  the patch is applied: `git -C _Build/linux-no-qt/_deps/zarchive_source-src apply --check --reverse
  --ignore-whitespace 3rdparty/patches/zarchive-reader.patch` must succeed; if not, apply it
  (`... apply --ignore-whitespace ...`) and rebuild, or archive_file fails. CMake left unchanged on
  purpose: the owner's offline Windows deps cache is not in this checkout.
- Linux build breakages found and fixed: Tracy link (eb8539c), allocSampler throw (b2d1654),
  Windows-only config call (1d2544c), avplayer timer link and non-copyable Pipeline in tests (625aed4).

## Status (at edfa80d)
- Milestone A: Linux build green; ctest 47/50 on lavapipe (1 environment-blocked, 2 pre-existing tests
  that encode upstream behaviour, R7). GPU lane runs; sync validation clean. CLOUD-TEST-RESULTS.md.
- Milestone B commits: fault report (d8b8cc9, 1d2544c), tiler buffer range checks (b2f8985), prewarm
  integrity (e34c2ea, f1061f8), low-memory guard (04a0fdd), PRT tiler fixed-element builds (3b4c5a0),
  download staging WAW barrier (df14c37), trim cooldown (512cc22).
- Milestone C: persisted-cache audit done. Milestone D: differential SRT harness (d0d6aa8); W3b waits for
  a current dune profile (recipe 2).

## Next (in order)
1. Review opt-in texture_quality=reduced (46e2a7d) end to end (W4.1): mip skipping in views, samplers,
   copies, promotion; skip detiling dropped levels only if the tiler can select them.
2. Opt-in RAM-burst attribution counters (W4.3): bytes the emulator first reads from never-touched guest
   ranges per 10 s, to separate emulator-caused residency from the game's.
3. DS loading (W5.2): bounded directory metadata cache behind a flag, with a temp-dir test.
4. W3b compiled SRT program only after a dune profile of the current build.
