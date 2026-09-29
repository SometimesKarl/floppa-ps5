# Cloud session log

Format per commit: date/time, commit, work item (W#.#), change, evidence, verification (Linux build,
Windows build if possible, unit tests, review), risk, flag, recipe entry.

## Starting point (local session ended 2026-09-29 ~21:00)
- HEAD 9c52585 on `local/merge-upstream-0929` (upstream KytyPS5 main merged to 59a1760).
- Builds on Windows clang-cl (`perf-handoff/build/build-clangcl-release.cmd`; zstd and ZArchive from a
  local deps cache on the owner's PC). Linux build not tried for our changes yet: do that first.
- Build of HEAD for the owner: `perf-handoff/builds/9c52585/` (exe, manifest, zipped pdb).
- Next: W1.1 (re-entrant fault handler), then the rest of the backlog in order.

## Local follow-up after the handoff (2026-09-29, owner's PC)
- e49dd65 (W1.1 done): fault handler no longer re-enters while reporting (thread-local guard, single
  line then std::_Exit(3)); host backtrace walk uses the no-unwind-data leaf guess only for the first
  frame and checks 512 readable bytes at rsp before each RtlVirtualUnwind. Built on Windows; staged as
  `Performance Experiments/builds/e49dd65-clangcl`. Not exercised (needs a fault). Next agent: W1.2.

## Cloud session 2026-09-29 (Linux container)

### W1.2: S20 guest fault analysis (analysis; diagnostics uncommitted at time of writing)
- Correction to the earlier description: the fault is not in the bit-scan loop. Disassembly of the
  report's code bytes (base pc-48) ends in
  `mov rax,[rsp-0x10]` / `mov [rax+0x18],r12d` (pc 0x900b3c079), then the epilogue `pop rbx..rbp; ret`.
  rax=0 so the guest stored to address 0x18: a null pointer that was reloaded from the red zone
  (below rsp). The lzcnt/blsi bytes only precede it in the same function.
- Source-supported mechanism: on Windows a vectored exception runs on the faulting thread's own
  stack (common/hostException.cpp), which overwrites the 128 bytes below rsp that SysV guest code
  uses. Linux uses SA_ONSTACK (comment there: GPU tracking can make guest stacks read-only).
  `red_zone_protection_enabled` defaults to false (common/emulatorConfig.h) and no handoff file
  mentions `--redzone`, so S20 most likely ran without it (not confirmed).
- Hypotheses, unproven: H1 red zone clobbered by an earlier recoverable host exception on this
  thread; H2 the guest really produced a null (HLE result or data race). The report did not dump
  memory below rsp, so H1 could not be tested from S20.
- Change: the fault report now prints `red zone protection: enabled|disabled` and the 128 bytes
  below rsp (bounded by IsReadableRange). Fatal path only. Not exercised (needs a fault); Linux build
  pending. No unit test: the code sits in a function that ends the process.
- Recipe: rerun DS with `--redzone`; if the BPE job-worker fault stops, H1 is supported. If it
  recurs, read the below-rsp dump: CONTEXT/EXCEPTION_RECORD patterns (0xc0000005, mxcsr 0x1f80)
  there confirm clobbering.

### W0 findings
- Linux build failed at first on the two standalone test targets (missing Tracy link); fixed.

### W2.1: review of 9c52585 (source review only, no code change)
- Sound: the extra ShaderRead bit only widens the destination scope of a barrier that is emitted anyway,
  so it cannot drop a dependency. Read-only depth aspects sampled in a read-only layout are handled
  correctly by ReadAlreadyVisible (image.cpp). PrepareBindings requests
  `attachment_access | ShaderRead [| ShaderWrite if binding.shader_write]`, equal to the new
  `transit_access` unless the target is also written as storage.
- Limit: in the writable-stencil layout (DS D32S8) the recorded state after a sampling draw is
  attachmentRead|Write|ShaderRead. A later draw that does not sample the target asks for
  attachmentRead|Write, which is not read-only, so Transit still emits a barrier and ends the pass.
  If S20's 673/s came from sampling and non-sampling draws alternating, 9c52585 removes only part.
  Candidate follow-up (conservative, adds only a read bit): request ShaderRead on every attachment
  transit of a depth target that any draw of the pass may sample. Not implemented: needs the local
  KYTY_GPU_STATS result of recipe entry 1 first.
- Pre-existing, not from this commit: AppendBarriers counts only Transfer/Shader/Memory writes as
  `repeated_write`, so two identical attachment-write requests across a render-pass boundary emit no
  barrier. Relies on driver ordering between pass instances. Hazard unproven; audit with the W4 model.

### W1.3: tiler and upload bounds audit
- TileManager::Prepare already validates every dispatch (overflow-checked arithmetic, work-group limits,
  linear/tiled ranges against the stated capacities). Gap: no entry point can see the real VkBuffer size,
  so `offset + capacity` beyond the buffer went unchecked.
- Change: `CheckBufferRange` in textureCache.cpp stops with a message (same policy as CheckCopyRegions)
  in UploadImage, DownloadDepth and DownloadImage. Risk: a false positive would end a game that ran
  before; ledger says CheckCopyRegions had none in S18-S20, but this check is new. Recipe entry added.

### W2.2: color clear access (change, unverified at runtime)
- DCC conditional clear (`ClearColorIfPredicate`) already requests attachmentRead|Write, same as draws
  (renderDraw.cpp:491): nothing to change there.
- `ClearImage` aliased-format path transited with ColorAttachmentWrite only, so a following draw
  (Read|Write) was an access-only barrier that ends the pass. Now requests Read|Write. Provably
  equivalent apart from one fewer barrier: destination scope widened by a read bit, stage mask unchanged.
- Not the cause of every "ColorAttachment->ColorAttachment" break (other producers not audited).
- Common clear path (clearColorImage in TRANSFER_DST) still needs a layout barrier before the next
  draw. Folding such clears into the next pass's loadOp would remove it but reorders commands: design
  candidate, not started.

### Linux build fixes (all pushed)
- eb8539c Tracy link for standalone resource tests; b2d1654 allocSampler abort when exceptions are off;
  1d2544c my d8b8cc9 called the Windows-only Config::RedZoneProtectionEnabled unguarded (fixed);
  625aed4 avplayer_file_tests links loader/timer.cpp, ShaderRecompilerComputeTests fills caller-owned
  PipelineCache::Pipeline (non-copyable since e9856e3 added std::atomic<bool> ready).

### Milestone C audit: persisted shader/pipeline data
- Driver cache (`<title>.bin`): sound. Signature (vendor, device, driver version, pipelineCacheUUID) +
  XXH3 of payload + 512 MiB bound; driver rejection falls back to empty; temp file + rename.
- Precompile records (`<title>.shaders`): bounds-checked reader, torn tail truncated before append,
  replay skips records whose static key no longer matches. Gaps (not changed): no per-record checksum
  (a flipped bit inside GCN code would be translated; the recompiler EXITs on what it cannot decode);
  whole file read into memory without a size bound. A checksum needs a format version bump, which
  discards every owner's recorded set once; not worth it without evidence of corruption.
- Prewarm records (`<title>.pipelines`): module hash was never verified on load, so a damaged file fed
  arbitrary SPIR-V to the driver at boot. Fixed in e34c2ea (DecodeModuleRecord + 105-check unit test,
  ASan/UBSan clean). Pipeline records are identified by their XXH3 but not checked; ParseGraphics/
  ParseCompute are bounds-checked (Reader::Ok requires the whole payload consumed).
- Worker counts (CompileWorkerCount, PrewarmThreadCount): clamped, hardware_concurrency()==0 safe.

### Milestone D (W3a) review
- Walk scratch is already per plan and reused (active_sources, visited_blocks, pending_blocks,
  descriptor/flat/condition roots); no per-walk allocation left in FindActiveSources/RefreshFlatBuffer
  after the first walk of a plan. References into plan vectors held across evaluation
  (`roots[source]`, `condition_roots[index]`) are safe: those vectors are resized once, to full size.
- Two SrtWalker objects per materialization are cheap (references + generation bump).
- Next for D: W3b compiled program needs a differential harness first (ResourceMaterializationTests).

### W1.2 follow-up: why the red-zone hypothesis is the lead
- `--redzone` (Config red_zone_protection_enabled, Windows only, default off) makes
  loader/redZonePatcher.cpp (from shadPS4) relocate memory instructions inside guest functions that
  keep locals below rsp, so that a recoverable host fault on them cannot let Windows exception
  dispatch overwrite those locals. `--amd-cpu` also runs the patcher but only for rsqrt emulation
  (protect_memory stays false).
- The S20 function fits the pattern: epilogue `pop rbx, r12..r15, rbp; ret` with no `sub rsp`, and a
  local reloaded from [rsp-0x10]. The load between store and reload that could fault is
  `blsi rbx,[rsi+rax*8]` (reads a bitmask); a recoverable read fault there happens when the page is
  read-protected for GPU readback tracking. Still unproven: we have no evidence a fault occurred on
  that thread before the crash. Recipe entry 10 gives the decisive test.
- Not changed: making --redzone the default would patch guest code for every title; keep it opt-in
  until the S21 run.
- EmergencyCollect / RunGarbageCollector reviewed (W4.4 old plan): scans are bounded (8192 / 4096),
  kept images are touched to the young end so they are not rescanned every tick, write-backs are
  bounded by EvictionDownloadMax * 2 per collection. No change.

### W3 baseline: differential SRT tests (d0d6aa8)
- resource_materialization_tests gains a branch plan over logged fake guest memory: untaken-arm
  pointers never read, taken-arm read failure fails the walk, undecided branch keeps both arms, and
  2000 seeded walks (seed 0x5eed1234) alternating decoded/interpreted evaluation on one plan with
  memory and branch changing. Mutation-checked (no generation bump; conditions never decided): both
  caught. -O2 and ASan+UBSan+LSan clean. This is the harness any W3b compiled program must pass.

### MaterializeMemo review (pipelineCache.cpp MaterializeCached): no defect found
- Capture covers both walkers: strict reads through CapturingStrictRead (the clean walker's
  read_memory via CleanRuntime), direct reads and user-data reads through the thread-local observers.
- Validation compares user data first, then re-reads strict ranges (including the ok flag) and direct
  words with the same reader order as ReadRaw. shader_base and user-data count are part of the key.
- A memo hit skips the walk's side effects; the only one, plan.specialization_reads, is consumed
  inside the same walk (WrittenBuffersDisjoint), so no stale state leaks.
