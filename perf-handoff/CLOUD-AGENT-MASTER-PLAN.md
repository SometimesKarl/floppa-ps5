# Cloud agent master plan: KytyPS5 efficiency work (no game tests available)

Read `perf-handoff/FINDINGS.md` first (measurements, root causes, commit ledger), then this plan.
History and older plans: `perf-handoff/docs/` (WORKLOG.md is the detailed run log).

## 0. Mission and how to spend the budget

Make KytyPS5 run ASTRO BOT (PPSA21564) and Demon's Souls (PPSA01342) faster and more stably on a
Ryzen 5 5500 / RX 6650 XT 8 GB / 16 GB RAM PC: fewer per-draw CPU costs on the GPU command thread,
fewer render-pass breaks and transfers, safe VRAM and RAM use, no crashes.

You cannot run the games or a GPU here. Your output is **code that compiles, is covered by unit tests
where the logic allows, is reviewed for correctness line by line, is opt-in when behaviour could
change, and comes with an exact test recipe** for the owner's next local session.

Budget rule: the owner wants the full cloud budget used on real, deep engineering work. Keep working
through the backlog below in order; when an item is blocked or finished, take the next one. Do not stop
after one item, do not pad with cosmetic edits, do not rewrite documents instead of code. Commit after
each coherent step (small, reviewable commits with the reasoning and evidence in the message). Stop only
when the budget is exhausted, and leave `perf-handoff/CLOUD-SESSION-LOG.md` current at every commit so
work can resume anywhere.

## 1. Rules (non-negotiable)

1. Never obtain speed by skipping draws, dispatches or effects, lowering quality silently, or losing
   guest data (GPU-written images must be written back before eviction; guest memory contents are
   never discarded).
2. Anything that can change rendering or timing and is not provably equivalent goes behind an opt-in
   switch (env `KYTY_*` and/or `emulator-settings.ini` key parsed in `src/main.cpp`
   `ApplyEmulatorSettings`) and is listed in the test recipe. Provably equivalent refactors may be on by
   default when a verify mode exists (pattern: `KYTY_SRT_VERIFY`, `KYTY_CLEAN_READ_VERIFY`).
3. Keep the branch building on **Windows clang-cl** (owner's platform; `perf-handoff/build/`) and on
   **Linux clang** (your platform). Guard platform APIs with `#if KYTY_PLATFORM == ...`.
4. Keep upstream mergeable: KytyPS5/KytyPS5 `main` moves daily. When merging, keep our pipeline-cache
   mutexes (prewarm workers and async compiles insert concurrently) and our bounded RELEASE_MEM flush
   coalescing (`CommandProcessor::BufferFlushCoalesced`, 1 ms / 32 labels). Never push to the upstream
   repository.
5. Do not claim performance numbers. Write "expected" with reasoning, and put the measurement in the
   test recipe. The previous session's rule: report measured, verified and unverified separately.
6. Match the surrounding code style (tabs, clang-format settings in the repo, comment density: explain
   why, cite the evidence run, e.g. "DS S20: 673 of 831 passes a second").
7. Heredoc/scripted edits corrupted `"\n"` inside C string literals several times: after any scripted
   edit, grep for broken string literals before building.

## 2. Build and test environment (Linux container)

```bash
sudo apt-get install --no-install-recommends clang lld ninja-build cmake git glslang-tools pkg-config \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev libxrandr-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
  libwayland-dev wayland-protocols mesa-vulkan-drivers vulkan-tools
git submodule update --init --recursive
cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests --parallel
```
`mesa-vulkan-drivers` provides lavapipe (CPU Vulkan), so Vulkan-dependent tests may run
(`tests/ShaderRecompilerComputeTests.cpp` skips rasterization cases on limited GPUs). Individual test
targets exist (`resource_materialization_tests`, `resource_tracking_tests`, `memory_tracker_tests`,
`image_page_table_tests`, `page_manager_tests`, `virtual_memory_allocation_tests`, ...; see
`CMakeLists.txt` from `add_kyty_full_emulator_test`). First task: make the branch build and the existing
tests pass on Linux; fix anything our Windows-only session broke there.

## 3. Architecture notes you need

- **GPU command thread** (`src/graphics/guest_gpu/graphicsRun.cpp`, `command_processor/pm4Handlers.cpp`)
  parses PM4 packets in order and records Vulkan (`src/graphics/host_gpu/renderer/`). Per draw:
  `RenderExecutor::PrepareDrawRenderState` (renderDraw.cpp) -> `RefreshShaders` ->
  `PipelineCache::GetGraphicsPrograms` -> `ProgramCache::MaterializeCached` (pipelineCache.cpp) ->
  `ShaderRecompiler::IR::MaterializeResources` (ir/passes/ResourceMaterialization.cpp, SrtWalker.cpp) ->
  then `ExecutePreparedDraw` -> `PrepareGraphicsBindings` (pipeline/descriptors.cpp, texture/buffer cache
  lookups, uploads) -> `AcquireRenderTargets` -> Vulkan recording.
- **SRT walker**: per shader plan (`ResourcePlan` in ir/ShaderIR.h, extracted per program), two walkers per
  walk: clean (strict reads through `ReadShaderGuestMemory` -> `Memory::TryReadGpuCleanBacking`) and
  observed (direct reads). Decoded-node evaluator `SrtWalker::EvaluateNode`, per-walk memo via
  `EvaluationContext` generations. `Inst::EvaluationIndex` assigns indices lazily, so `srt_nodes` can
  grow mid-walk: **never keep references into it across evaluation** (7fd5120 broke rendering that way).
  Memo of whole materializations keyed by captured reads (`MaterializeMemo`), bypass after 16 misses.
- **Texture cache** (cache/textureCache.cpp): images keyed by guest range and layout; `FindImage`
  resolves overlaps (`ResolveOverlap`); `InitializeImage`/`UploadImage` upload from guest memory
  through the buffer cache and the tiler (image/tiler.cpp, GPU detile into a scratch buffer, then
  copyBufferToImage); render-target band uploads (`ChangedRowBand`); GC with LRU, `EmergencyCollect`,
  `ReclaimForAllocation`; `ImageInfo::host_mip_skip` for reduced quality.
- **Image state/barriers** (image/image.cpp `AppendBarriers`, `Transit`): whole-image or per-subresource
  state; a barrier inside a render pass ends it (`m_scheduler.EndRendering()`), counted by
  `KYTY_GPU_STATS` ("rendering ended by", including "transit OLD->NEW WxH FMT").
- **Buffer cache** (cache/bufferCache.cpp): guest ranges mirrored in device-local buffers,
  `SynchronizeBuffer` uploads CPU-dirty ranges (ends the render pass), GPU-dirty tracking, downloads.
- **Guest memory** (src/kernel/memory.cpp, memoryAddressSpace.inc): SEC_COMMIT pagefile section mapped
  at guest addresses plus one backing view; `TryReadBacking`, `TryGetBackingPointer`, map generation,
  `GpuCleanReadScope`, `TrimGuestWorkingSet`.

## 4. Backlog (do in this order; each item: design note in the commit, code, tests, recipe)

### W1. Crash and freeze safety (small, first)
1. **Re-entrant fault handler**: `Loader::KytyExceptionHandler` (src/loader/runtimeLinker.cpp ~673-768)
   re-entered 5 times because `PrintHostBacktrace` faulted inside ntdll (DS S20). Make the handler
   detect recursion (thread-local depth), skip host stack walking on re-entry, write the guest context
   first, flush, and terminate cleanly. Unit-testable pieces: the recursion guard.
2. **S20 guest fault analysis**: guest pc 0x900b3c079 in a job worker (see logs/S20-crash-report.txt:
   registers, code bytes). Decode the bytes (bsr/lzcnt/blsr bit-scan loops over a bitmask with
   rcx=-1, rbp=1, rax=rbx=0) and reason which HLE result could have produced them (job system: pthread
   / condition / semaphore / event flag / memory query functions in src/kernel, src/libs). Document
   hypotheses; add bounded diagnostics (opt-in) that would confirm them next run.
3. **S17 freeze follow-up**: audit `UploadImage` multi-band path (textureCache.cpp, only with
   KYTY_PARTIAL_UPLOADS_EXT=1) and upstream #894 (block-compressed images with storage usage) for any
   out-of-range tiler dispatch or copy; extend `CheckCopyRegions` (image.cpp) style checks to tiler
   dispatches (tiled/linear ranges vs buffer sizes) before recording.

### W2. Render-pass breaks and barriers (package 5; biggest proven DS lever)
1. Review 9c52585 (depth target sampled read-only uses one combined access) for correctness against
   `DepthReadableAspects`/`DepthWritableAspects` and the descriptor path in descriptors.cpp (~1100).
2. Color targets cleared through `ClearColorIfPredicate` / the clear path leave access
   `ColorAttachmentWrite`; the next draw asks `ColorAttachmentRead|Write` -> barrier + pass end (DS:
   "transit ColorAttachment->ColorAttachment 1920x540 / 214x120"). Make the clear paths leave the same
   access the draw path requests, or treat attachment-only access changes on the same image as
   no-ops when the image is an attachment of the active pass (needs the scheduler's current attachment
   set; `CommandBuffer::IsRendering()` exists).
3. Buffer uploads inside a pass (`BufferCache::SynchronizeBuffer`, streamBuffer.cpp) end it. Design:
   record uploads for the draw into a separate "pre-pass" command buffer (or before `BeginRendering`
   via the prepared-draw split: bindings are resolved before the pass starts), preserving ordering with
   earlier GPU writes to the same buffer (GPU-dirty ranges must not be overwritten). Opt-in flag.
4. Global barriers from guest RELEASE_MEM / EVENT_WRITE (graphicsRun.cpp `EmitGlobalBarrier`, ~280/s at
   the DS cinematic, 1,283/s in S12 gameplay): research whether a pending global barrier can be deferred
   until the next command that could observe the ordering (compute, copy, new pass, storage write),
   keeping the guest's completion events exact. Write the analysis first; implement only with a verify
   mode.

### W3. Cheaper draws (packages 2 and 3; dune and DS)
1. **Compiled SRT program** per plan: after the first complete walk, build a flat, index-stable
   operation list (post-order) for the nodes each consumer root needs, with operand indices resolved,
   and evaluate lazily per root without the recursive decode/copy path. Keep `KYTY_SRT_VERIFY`
   equivalence (IR interpreter comparison) and add unit tests in `tests/ResourceMaterializationTests.cpp`
   with synthetic plans (reads, conditions, descriptor roots, failing strict reads). Must not evaluate
   nodes a consumer does not reach (unreached pointer chains can fault).
2. **Walk scratch**: `FindActiveSources`/`RefreshFlatBuffer` reassign vectors per walk; reuse, avoid
   `std::vector::at` in hot loops after one-time validation (already done for FindActiveSources base).
3. **Descriptor/binding preparation**: measure in code which parts of `PrepareGraphicsBindings`
   repeat identical work across consecutive draws with the same program and resource set (texture
   views, sampler lookups, buffer offsets); cache per (program, resource generation) with invalidation
   on texture/buffer cache changes (add generation counters). Opt-in until verified.
4. **Hot-path allocations** from the X52 profile: `InlinePageOwnerList<SlotId,128>` constructors,
   `Common::BitArray<1024>` constructions, `std::_Tree` lookups in `GuestBackingStore` (partly fixed by
   d54c0f9 / 8df78aa), `TextureCache::FindImagesInRegion` per texture per draw: add a small
   descriptor->ImageId cache validated by a texture-cache generation counter.
5. **Tiler scratch per upload**: `TileManager::Detile` allocates and defer-destroys a new scratch
   VkBuffer for every upload (image/tiler.cpp). Replace with a ring or pool with fence-based reuse;
   consider detiling directly into the image through a storage view where the format allows
   (halves upload bandwidth; DS moves ~30 GB per 10 s).

### W4. VRAM and RAM
1. Review `texture_quality=reduced` (46e2a7d) end to end; add detile-skipping for dropped levels
   (currently level 0 is still detiled into scratch); make `AccountedSize` reflect host bytes.
2. `texture_ram=trim`: S20 still trimmed 17.7 GB in ~10 min, so CPU-written textures are re-uploaded
   repeatedly. Find which path marks them CPU-dirty (page-granularity maybe-dirty, hashing in
   `RefreshImage`), avoid trimming images uploaded more than once recently, and stop repeated uploads
   whose content hash is unchanged.
3. RAM bursts (2-4 GB in ~10 s at DS area loads): add opt-in attribution (per-10 s bytes the emulator
   itself reads from never-touched guest ranges: first uploads of render targets / GPU-only images,
   buffer creation syncs) so the next run can tell emulator-caused residency from the game's.
4. Emergency eviction (0521196): review for pathological rescans when nothing is evictable; bound work
   per GC tick.

### W5. Demon's Souls CPU contention and loading
1. Guest job workers: find the HLE calls their idle loop makes (kernel sleep/yield/cond wait; upstream
   6f24b03 made short sleeps block) and make sure none busy-waits on the host; keep guest-visible timing.
2. Loading: APR/open/stat paths on loose files (src/libs/libAmpr.cpp, src/kernel/fileSystem.cpp):
   batch directory metadata (one enumeration per directory) behind a flag.

### W6. Parallel draw preparation (only after W2/W3)
Two-worker prototype behind a flag: the command thread runs ahead, snapshots per-draw walk inputs
(user data, shader addresses), workers compute materializations with per-thread scratch, the command
thread validates them with the existing memo re-read check and records in order. Plan state must be
thread-safe first (no lazy growth of shared plan vectors during worker walks). Expected ideal gain for
walk-only parallelism is <= 1.34x on the dune (34% of the thread); document measured overheads.

### W7. Native 1080p research (docs/NATIVE-1080P-PLAN.md)
Scene/depth/history/post resource mapping for both games; keep the legacy DRS steering (desert 23 -> 37
FPS) until a verified replacement exists.

### W8. Keep upstream current
Merge KytyPS5 `main` periodically (rules in section 1.4), rebuild, rerun tests.

## 5. Test recipe for the owner (append to it as you go)

Maintain `perf-handoff/LOCAL-TEST-RECIPE.md`: for each change, the build, env vars, route (ASTRO:
`tools/route_desert.sh` + `tools/travel.sh 4` or `tools/ab_dune.sh`; DS: `tools/drive.py launch` with
DRIVE_GAME=S:/.../PPSA01342-app0/eboot.bin, DRIVE_PATCH=none, DRIVE_NO_VALIDATION=1,
DRIVE_TEMPLATE=testbed/ds-fresh, KYTY_TEXTURE_RAM=trim so gameplay fits in 16 GB), what to read in the
logs, and the pass/fail criterion. Always run with `tools/ram_watchdog.py 1024`.

## 6. Session log format (perf-handoff/CLOUD-SESSION-LOG.md)

For each commit: date/time, commit hash, work item (W#.#), what changed, why (evidence), how verified
(build Linux/Windows?, unit tests, review), risk, flag name, recipe entry. At the end: what is next.
