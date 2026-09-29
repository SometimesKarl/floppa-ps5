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
- Re-configuring an existing build dir fails in the zarchive FetchContent patch step (non-idempotent
  `git apply`, 3rdparty/CMakeLists.txt). Workaround before any reconfigure:
  `git -C _Build/linux-no-qt/_deps/zarchive_source-src checkout -- .`
- Linux build breakages found and fixed: Tracy link (eb8539c), allocSampler throw (b2d1654),
  Windows-only config call (1d2544c), avplayer timer link and non-copyable Pipeline in tests (625aed4).

## Status
- W0 build: running; tests not yet run (see CLOUD-TEST-RESULTS.md once done).
- Next exact step: after the build finishes, reconfigure (workaround above), rebuild with `-k 0`,
  run `ctest --test-dir _Build/linux-no-qt --output-on-failure --timeout 300 -j1`, record counts.
