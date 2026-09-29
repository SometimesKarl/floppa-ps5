# True 1080p rendering and output — implementation plan

Prepared 2026-09-28. Status: researched plan; the changes below are not implemented by this document.

This is the resolution and shader workstream for DEVELOPMENT-PLAN.md / OPUS-AUTONOMOUS-MASTER-PLAN.md. Their correctness, process ownership, save/cache preservation and deployment rules continue to apply. Latest user preferences: 190 Hz display, frame cap off, 1080p as the primary mode, no upscaling to 4K. ASTRO BOT and Demon's Souls require separate compatibility profiles.

## 1. Deliverable and meaning of “1080p”

Deliver a mode in which the primary scene is rendered at 1920×1080, the final game composition is produced at 1920×1080, and the emulator presents that result without first constructing or reconstructing a 4K screen image. Retain the intended game effects, UI, particles, lighting, simulation and sound.

Track four different dimensions independently:

1. **Scene resolution:** primary color, depth and associated scene buffers. The native 1080p target is 1920×1080. Legitimate half/quarter-resolution effects remain proportional to this target.
2. **Composition resolution:** post-processing, temporal history, game UI and the final game image. The target is 1920×1080; no hidden 1080p-to-4K reconstruction followed by downsampling.
3. **Guest display-buffer description:** the dimensions and layout the game registers with VideoOut. Prefer genuine 1920×1080 guest buffers. If a future emulator override retains a logical 4K guest description but renders a physical 1080p representation, disclose that explicitly.
4. **Host drawable/swapchain:** the actual window pixels. Existing tests use a 1280×720 window, so a 1080p game image is downsampled for those tests. A 1920×1080 drawable is needed to verify 1:1 presentation; do not silently enlarge test windows or claim their swapchain is 1080p.

“Native 1080p complete” requires the first two dimensions to be 1080p and a verified final presentation path. A smaller window, smaller presenter copy, or 1080p scene followed by 4K post-processing is an intermediate result. Same-resolution TAA is allowed; reconstructing from a lower scene resolution is a separate optional mode. FSR/XeSS/frame generation are outside this workstream.

Asset textures, shadow maps, cubemaps, lookup tables and simulation buffers may legitimately exceed 1080p. Do not indiscriminately shrink every 3840-wide image or every large allocation.

## 2. Current evidence and its limits

### Inspected source and deployment

- Source is `C:\Users\himav\Desktop\kyty ps5-src`, branch `local/astro-perf-up`.
- Initial source snapshot was `c837382` with unrelated file-I/O edits. Other work advanced HEAD to `3e6ecc8` during inspection. Reconcile again before implementation; this document does not own or modify those changes.
- `Optimized Build/build-manifest.txt` identifies deployed build `78256f4`; this inspection did not hash the deployed executable anew. Its settings select `render_resolution=1080p`, `frame_cap=off`.
- `resolutionControl.cpp` steers a reference clock to influence ASTRO's DRS. Its current profile checks title identity; a different version prints a warning but is still accepted. Patch fingerprint enforcement is absent from that path. This is a compatibility heuristic, not a native output-resolution implementation.
- `VideoOutGetOutputStatus` in `presentation/videoOut.cpp` reports 4K by default unless the title's ATTRIBUTE3 resolution-detection flag and context size select the smaller mode.
- Historical `X23-res1/emulator-stdout.log` reports resolution code 1 (1080p), yet ASTRO registers **two 3840×2160 display buffers**. This disproves that changing this return value alone fixes the tested game. It does not prove that every other output negotiation path was tested.
- `Presenter::PrepareFrame` in `presentation/window/swapchain.cpp` acquires a frame from the guest extent, then configures it to the resolved source backing extent. `Frame::CopyFrom` uses `copyImage`; its min-dimension extent is a crop if dimensions differ, not a rescale. Both allocation/configuration and transfer must change together for a smaller presenter image.
- Host swapchain extent follows the window/surface. It is not universally hardcoded to 4K. The game-provided source and presenter intermediate are the demonstrated 4K costs.
- `ImageInfo` carries guest range, extent, pitch, tiling, samples, mip layout and metadata together. A generic host scaling implementation must separate logical guest layout from physical host layout.

### Performance evidence

- ASTRO's historical DRS steps include 1920, 2432, 3328 and 3840 pixels wide. The setting named 1440p currently aims near 2432×1368, not exact 2560×1440.
- Historical desert figures near 37 FPS versus 23 FPS demonstrate that reducing scene pixel work can help. They are not a new controlled comparison and do not predict the improvement from removing the remaining final 4K passes.
- Dune profiles show substantial per-draw CPU work. Thousands of small particle/sand draws remain costly even if each shades fewer pixels.
- The user reports that Demon's Souls reached tutorial gameplay with HUD, rain, lighting and character rendering correctly. S12's final log contains roughly 266–297 ms frames, with about 192–205 ms in the residual `other` category, 19–22 ms buffer work, and 43–64 ms GPU waits. The reported roughly 5,000 indirect draws/frame is consistent with a serious draw-processing bottleneck; `other` alone does not attribute all 200 ms to the resource walker.
- S13 exists, but the inspected tail shows a different workload and is not evidence of improved tutorial performance. Reach and confirm the same scene before comparing it with S12.

These observations justify two parallel tracks: remove unnecessary screen-resolution work, and reduce CPU preparation/compilation cost. Neither substitutes for the other.

## 3. Expected savings, without an FPS promise

3840×2160 contains 8,294,400 pixels; 1920×1080 contains 2,073,600. For a genuinely resized screen buffer with unchanged format/sample count, logical pixel storage drops by 75%.

- One single-sample RGBA8 or RGB10A2 surface: approximately 31.64 MiB → 7.91 MiB, saving 23.73 MiB.
- One single-sample RGBA16F surface: approximately 63.28 MiB → 15.82 MiB, saving 47.46 MiB.
- Two RGBA8 guest display buffers would save approximately 47.46 MiB if the guest genuinely allocates them at 1080p. A pool of N additional RGBA8 presenter frames could separately save N × 23.73 MiB.

These are mathematical payload estimates before alignment, compression, tiling and allocator overhead. Count actual live allocations once, including overlap and deferred destruction, before estimating total savings. Guest address-space reservations may remain unchanged under host-only scaling.

For each captured frame, estimate savings from its eligible color/depth/history/post-process allocations and measured pass durations. Report what fraction of the working set is eligible. Do not promise 75% less total VRAM: asset textures, geometry, compiler memory, guest heaps and many shadows remain.

Rendering fewer pixels can reduce fragment shading, image traffic and correctly resized screen-space compute. It normally does not reduce the number of shader programs, make a huge shader compile four times faster, or remove per-draw CPU processing. Texture sampling traffic may decrease, while uploaded asset bytes remain similar unless the game's streaming decisions change.

For reasoning only, use `max(CPU critical path, GPU work)` plus synchronization/pacing as a first approximation to frame time; do not sum overlapping CPU/GPU durations. Measure the new critical path. A 200 ms serial CPU path still prevents smooth gameplay after a large GPU saving.

## 4. Resolution strategy and decision gates

Use the least invasive path that changes the whole screen-rendering chain correctly.

### R0 — map the actual frame and establish identities

**Mechanism:** current width/bind counts do not distinguish expensive scene passes from UI, copies or reused resources.

Add an opt-in, bounded capture for selected frames containing:

- Title/version, executable and patch fingerprints, source/build identity, settings, driver, cache namespace and scene marker.
- Guest display dimensions; guest and host image extent; format; mip/layer/aspect; samples; pitch/tiling; allocation bytes; content generation and alias identity.
- Producing draw/dispatch/clear, consuming passes, shader recipe/hash, viewport/scissor, dispatch dimensions and indirect argument source.
- A small set of GPU timestamp zones for candidate pass groups; collect results after completion without waiting every frame. Keep CPU timings separately. Avoid thousands of per-draw timestamp queries by default.
- Presented frame identity, host drawable size, intermediate sizes, transfer bytes, pipeline creation pauses and peak memory.

Trace backwards from each registered display buffer to identify the final composition, reconstruction, history, main color and depth resources. Identify whether effects use screen coordinates or operate on world/simulation data. Track aliases by lifetime/generation, not just address. Use a short ASTRO normal/dune capture and the reached Demon's Souls tutorial separately.

**Deliverable:** a pass/resource dependency map plus a ranked list of 4K work that survives the existing ASTRO 1080p setting. **Reject:** adding global per-draw scans or large recurring trace dumps. **Exit:** each candidate has known producers/consumers and a measured or explicitly unmeasured cost.

### R1 — audit guest output negotiation, once

Inspect the call order from startup through VideoOut status/support/configuration and display-buffer registration, including any title mode selection and param metadata. Record which values the game actually consumes and when it commits to buffer dimensions.

Try one coherent, title-scoped 1080p output capability/configuration profile only where the API contract is understood. Keep refresh, HDR/SDR and timing fields consistent with the chosen mode; do not change guest refresh because the monitor is 190 Hz. Do not edit installed param.json or globally report fabricated capabilities.

**Success:** the game itself allocates/registers 1920×1080 output buffers and its dependent pass sizes follow. **Stop condition:** another correctly configured run still registers 4K and the allocation path does not consume these reports. Record that result and move to R3; do not repeat the already failed numeric-enum sweep. If a real game graphics/output preference exists, inspect its behavior first; do not assume one exists for either title.

### R2 — reduce presenter overhead as an independent early improvement

Use the R0 measurement to decide whether this small change is worthwhile alongside R3. Add an explicitly identified presentation-copy mode that stores an already completed game frame at a bounded output extent, normally 1080p, independent of the test window's 720p drawable.

Implementation points in `swapchain.cpp`:

- Change FramePool acquisition, configuration and reuse criteria consistently; otherwise Configure recreates the image at 4K again.
- Replace the full-size copy with a supported filtered blit or dedicated reduction pass when dimensions differ. Check source/destination format capabilities. A smaller `copyImage` extent would crop the top-left of the game.
- Match color interpretation, sRGB/linear conversion, HDR handling, alpha/layer composition and pixel centers. Prefer a defined reduction filter over assuming every format supports a linear blit.
- Preserve the copied-frame snapshot and guest flip lifetime. Holding a guest display image directly after flip may race its next write; “zero-copy” is a separate ownership project.
- Preserve main/overlay layer alignment, resizing, blank frames, repeated presents and safe retirement of the old frame pool. Do not secretly reduce queue capacity.

**Expected effect:** smaller presenter allocations and changed presentation-copy cost. **Acceptance:** full image/HUD remains visible, layers/color match, resize works, queue semantics remain correct, actual copy/allocation metrics improve. Label this “1080p presentation from 4K source,” not native 1080p. It does not complete the user's request.

### R3 — preferred full solution: versioned game resolution profile

If R1 fails, trace the code/data that sets ASTRO's scene dimensions, DRS bounds, output dimensions and final history/UI targets. Start from observed buffer registration and allocation/descriptor creation callers. For Demon's Souls, discover its separate configuration path; do not reuse ASTRO offsets or clock steering.

Prefer a small emulator-applied, in-memory compatibility profile that lets the game build its own **internally consistent 1080p resource graph**:

1. Identify the complete resolution configuration and when it becomes immutable for a level/session.
2. Set primary scene dimensions to 1920×1080 and, where appropriate, fixed DRS min/max to that size. Set composition/output/history sizes coherently. Update inverse dimensions, screen grids, dispatch sizes and temporal parameters through their original game configuration path.
3. Keep equal-resolution antialiasing if valid. If the existing reconstruction pass requires unequal input/output sizes, supply a reviewed 1:1 variant or equivalent output stage that preserves its other effects; do not simply skip it.
4. Recompute allocation sizes, pitch and metadata using the game's own path. Registration-only descriptor changes after 4K commands were created are invalid.
5. Apply before dependent allocation/initialization. Start with restart-required settings. Handle history reset explicitly; do not switch under in-flight work.
6. Require exact title/version, executable/module fingerprint, renderer patch profile, expected original bytes/data and unique match count. Unknown builds fail closed with a clear unsupported-mode message. Keep installed game files unchanged and retain a reference mode.
7. Remove reference-clock steering from the accepted native profile. Retain it only as a separately labeled legacy option where verified; audit all clock consumers and reset behavior.

**Deliverable:** ASTRO native 1080p profile first, then an independently validated Demon's Souls profile. **Acceptance:** scene and final composition measured at 1080p, guest registrations preferably 1080p, intended content preserved, no altered game speed/audio, repeat level load succeeds. **Rejection:** only the reported size changes, any mismatched pitch/dispatch/history, unknown pattern matching, or gameplay-affecting patch effects.

This is a research task until the dimension-setting path is found. Do not promise that a safe small patch exists. If it does not, proceed to R4 with its larger scope documented.

### R4 — reusable emulator scaling, only for proven resource groups

This is the fallback architecture for titles that cannot be configured safely through R3. It is materially larger than changing a Vulkan extent. Vulkan stores image dimensions in the image allocation; a view selects compatible format/subresources and cannot freely resize it. [Image creation](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageCreateInfo.html), [image views](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html).

**Representation model:** preserve immutable guest layout/address/metadata information and attach a separate host representation with extent, scale, content generation, valid subresources, encoding, owner, aliases and retirement tick. Include resolution policy/ABI in relevant representation and cache identities. Do not halve `ImageInfo.extent` globally while retaining its original pitch/mip layout.

**Eligibility:** begin with one closed, title-identified group of single-sample screen resources whose every producer and consumer is understood. Match stable pass/resource relationships plus title/shader identity, not dimensions alone. Exclude atomics, exact integer data, readback, arbitrary buffer aliases, multisampling, complex mip/layer layouts and unknown consumers until individually supported.

Implement in coherent slices:

1. **Rasterization:** transform all used viewport/scissor slots, render areas, offsets and full/partial clears; handle odd dimensions and rounding explicitly. Map depth/stencil and color consistently. Preserve sample coverage and coordinate conventions.
2. **Shader addressing:** distinguish normalized sampling, integer fetch/load/store, image-size queries, fragment coordinates, derivatives and texel offsets. Guest-size queries may be correct for one logical path and wrong for a physically resized screen algorithm; define each pass contract. No universal global multiply is safe.
3. **Compute:** only rescale work dispatches proven to be screen-pixel processing with a complete shader/constant-buffer contract. Adjust bounds, coordinates, dimensions and neighborhood operations together. Preserve barriers/shared-memory participation. Never scale particle simulation, indirect argument generation, skinning, tiled light lists or other data workloads merely because their dimensions resemble a screen. Vulkan dispatch counts describe workgroups, not a rendering resolution. [Dispatch reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdDispatch.html).
4. **Temporal/post-processing:** keep depth, motion vectors, jitter units, history and reconstruction output consistent. Reset history on policy changes and test moving disocclusions, rain and dust. Classify intermediate effect resolutions relative to the pass, not just the final image.
5. **Copies, aliases and metadata:** define conversions at valid representation boundaries. Track generations and dirty ranges across buffer/image reads and writes. Do not interpret original 4K tiled offsets as 1080p offsets, or share DCC/CMASK/HTILE blindly. Guest metadata semantics and host compression remain separate.
6. **Presentation/UI:** consume the physical 1080p final representation without expanding it into another 4K presenter image. Scale UI geometry, clipping and input hit-testing consistently where needed. Preserve overlays and color conversion.

**Hard boundary:** a low-resolution render cannot reproduce exact lost 4K texels for an arbitrary guest buffer read. Expanding it back to 4K is not bit-exact preservation. If an exact-data consumer exists, leave that entire dependent group at guest resolution or use R3. Do not re-execute side-effecting draws as an improvised recovery. Decide eligibility before writes become authoritative.

**Acceptance:** one complete group passes deterministic coordinate/alias checks and moving gameplay checks before enabling another. A fallback is decided safely at creation/initialization, never after silently losing authoritative contents. Report any remaining high-resolution screen groups; mixed rendering is a partial milestone.

For architectural comparison, Xenia documents the complexity of render-target ownership and scaling on a different GPU. It supports investigating a representation model, not copying its console-specific rules into PS5 emulation. [Xenia architecture article](https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html).

## 5. Texture traffic and VRAM work paired with resolution

### M1 — account for the savings that actually exist

Separate scene/depth/history/UI/display/presenter memory from immutable assets, shadows, buffers, staging, compiler data, driver overhead and pending destruction. Compare actual live allocation bytes and driver budget, not just guest requested sizes. Log any full-resolution compatibility copy retained by R4; it may erase the expected saving.

Reducing guest allocations through R3 can reduce guest backing requirements as well as host targets. R4 may save only host representations. R2 saves presenter intermediates only. Keep these claims separate.

### M2 — remove repeated transfers with ownership evidence

Continue the existing partial-upload work independently of resolution. Trace the exact producer/consumer for repeatedly uploaded targets and distinguish conservative writable bindings from actual writes. Avoid re-uploading a current valid image representation. Skip preservation only before a proven full overwrite with all aspects/subresources covered and no intervening consumer.

Use tile/block-aware dirty ranges where mapping is proven; preserve untouched data. Validate the newly generalized tiling paths against the old full upload path. The historical intro striping remains a regression candidate until isolated; do not use an already damaged frame as the visual reference for scaling.

Prioritize screen-buffer aliases with high repeated copy cost. Keep BC asset textures compressed where supported. Do not discard high mips or lower texture quality implicitly. Reduced output resolution can change LOD demand, but cannot justify making shader-reachable texture contents unavailable.

### M3 — bound working set and transient peaks

Keep budget-driven high/low watermarks and preservation-aware retirement. Start from the governing plan's proposed reserve of max(768 MiB, 10% of driver budget), then size for observed transition bursts. Budget old plus new images during mode/load changes, staging and preserved copies; restart-only mode changes avoid an unnecessary live migration peak.

Use a finite allocation recovery path and a bounded system-memory fallback. Evict only reconstructible or correctly preserved resources after required GPU work retires. Detect repeated evict/recreate cycles of the same generation. Reserve capacity for progress-critical readbacks. VMA distinguishes reported usage/budget and provides budget-aware allocation behavior; a budget check is not a guarantee against all later pressure. [VMA budget guidance](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/staying_within_budget.html).

**Acceptance:** eligible allocations and transfer volume fall, memory plateaus, transition/revisit does not corrupt textures or thrash. Reduced resolution is not grounds to remove the process memory guard.

## 6. Shader optimisation: three separate costs

### S1 — reliable preparation and reuse

Make guest shader recipes, translated SPIR-V, full pipeline recipes and driver cache identities explicit. Include title/version/patch profile, translation/resource ABI, host features and any resolution-specific shader semantics. Do not unnecessarily invalidate dimension-independent shaders when only a dynamic viewport changes.

Validate lengths/counts/hashes/module references, recover truncated records, and bound parser memory. Preserve old compatible caches and use transactional or recoverable writes. A recompiler change requires replay from valid guest recipes; an old SPIR-V-only pipeline file does not automatically represent the new translation.

Begin with two preparation workers on this 16 GB system, constrained by measured compiler memory and current headroom; increase only if evidence supports it. Bound job queues and retained modules, prioritize required work, and suspend speculation under pressure. Report known recipes, cache hits, new states, errors and remaining foreground waits honestly. A driver-cache warmup that destroys executable pipelines does not mean later creation costs zero.

Compile every required graphics/compute pipeline before its dependent work executes. Preserve draw and dispatch semantics. First-ever unseen work can still stall; do not promise zero stutter from saved records alone. Keep accurate loading progress and cancellation. Historical GPL/spirv-opt experiments did not show a net win on this driver; revisit only with a changed hypothesis or driver, not as the default fix.

### S2 — reduce shader translation and GPU execution cost

Build a small corpus from the encountered ASTRO sand/dust/final-composition shaders and Demon's Souls tutorial shaders. Rank by foreground compile duration, call frequency, pass GPU time and resource pressure.

Investigate the observed long indexed-register selection chains, repeated descriptor/address calculations, EXEC predication, unnecessary conversions and dead outputs. Prototype a smaller indexed representation or predecoded resource program only where semantics and driver behavior support it. SPIR-V word count alone is not an acceptance metric.

Preserve divergence, helper lanes, derivatives, subgroup behavior, atomics, ordering, bounds and floating-point requirements. Do not replace guarded accesses with unconditional loads based solely on a robustness feature. Validate generated SPIR-V before submitting changed code in focused correctness runs; compare deterministic outputs and representative moving scenes.

For 1080p-specific variants, derive changed coordinates/constants from the R3/R4 pass contract. Prefer runtime scale parameters when they avoid pipeline permutation explosion without slowing the GPU. Specialize only when measured to help. Reduce screen work; do not skip effects or lower particle counts to inflate FPS.

### S3 — reduce per-draw CPU preparation, essential for both titles

The new Demon's Souls gameplay evidence makes this a parallel priority, not a late polish step. Sample the tutorial's GPU command thread directly and split residual time into resource walks, descriptor preparation, guest-memory access, command construction and driver calls. Use the same separation for ASTRO's dune.

Historical incremental SRT reuse was correct on a sampled corpus but slower and reverted. Start with a compiled linear/predecoded walk that reduces interpretation and allocation, using stable node indices and bounded scratch storage. Do not retain references into a vector that recursive decoding can grow; that already broke the menu. Preserve fallback and equivalence checking outside timed performance runs.

Reuse descriptor preparation only with proven program/state/content generations. Stable descriptor bytes do not establish unchanged backing contents or binding offsets. Avoid repeated decoding and allocations before attempting broad memoisation again.

Consider combining consecutive compatible indirect draws only after proving identical pipeline/descriptors/state and preserving draw order, DrawID semantics, queries, per-draw constants, dependencies and shader side effects. “5,000 draws” does not prove they can become one multi-draw command.

If a substantial serial cost remains, design a bounded prepare/record split with immutable snapshots, explicit resource ownership, ordered completion, command-pool ownership, cancellation and memory backpressure. Do not pass mutable guest pointers to delayed worker execution. Parallel compilation and command preparation must share a conservative CPU/RAM budget.

**Acceptance:** equivalent resources/outputs on covered tests, less CPU time per draw and shorter frame critical path without growing queue latency or memory. CPU utilization percentage alone is not the result.

## 7. Settings and compatibility contract

Proposed settings below are a design, not currently recognized configuration:

- `output_resolution=1080p|1440p|2160p|auto`: desired game composition/output size.
- `scene_resolution=1080p|1440p|2160p|auto`: desired primary scene size, bounded by verified profile capabilities.
- `resolution_method=auto|game_profile|legacy_drs|reference`: select a verified implementation; auto must disclose the method actually used and must not label a failed request “native”. R4 remains experimental until a profile qualifies.
- `upscaler=off`: default for the requested native mode; do not add FSR merely to downsample a completed 4K frame.
- `frame_cap=off`: preserve the user's choice. Host 190 Hz does not change the emulated vblank contract.

Migration: the existing `render_resolution=1080p` means legacy ASTRO DRS preference until a verified new profile is selected. Do not reinterpret it silently as proof of native 1080p. Show a concise startup/status message, for example “Scene 1920×1080; game output 3840×2160; window 1280×720” for current behavior, and the actual dimensions after implementation.

Scope profiles by title/version/executable/patch identity. Unsupported titles get a clear explanation and a reference path. Demon's Souls must not inherit ASTRO's clock manipulation. Initial mode changes require restart. Design exact 2560×1440 support using the same mechanism later; do not rename 2432×1368 to exact 1440p.

## 8. Verification and release gates

Use existing evidence before any new game launch. Plan preparation itself requires no game run. During implementation, stage candidates and copied saves/caches separately from the deployed build. Preserve Fixed Build entirely.

### Focused checks before game tests

- Coordinate/extent math: odd sizes, nonzero viewport origins, clipped/empty scissors, pixel centers, half-resolution chains, mips/layers, pitch and block alignment.
- Color/copy path: full frame rather than crop, correct sRGB/alpha, overlay alignment, resize, repeated presentation and snapshot lifetime.
- Resource logic: full and partial writes, alias generations, readback eligibility, retirement, allocation failure and unsupported-mode fallback.
- Cache logic: mode/ABI compatibility, malformed/truncated data, bounded workers and warm reuse.
- Rendering APIs: focused validation for changed paths; distinguish existing errors from regressions. Synchronization follows actual producers/consumers, including compute-generated indirect arguments. [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).

### Short runtime sequence

1. Confirm correct build/settings and enough RAM/commit/VRAM headroom. Record test PID, creation time and executable path. Start an owned-process guard with a conservative explicit 1536 MiB system/commit floor; reconcile older 450/1024 MiB tool defaults rather than inheriting them silently. Do not run beside a user's emulator.
2. **ASTRO:** menu and normal desert, then dune movement/jump/attack/dust; inspect a dark scene for known striping, HUD, depth intersections and temporal trails. Use a 30–90 s observed scene or one 2–4 minute action route as appropriate.
3. **Demon's Souls:** copied checkpoint or confirmed tutorial route, then movement/turning, character/HUD, rain, fog, lighting and combat if available. Record missing coverage if reaching it is impractical. Do not compare a cinematic S13 segment against S12 gameplay.
4. One targeted transition and revisit only after local correctness gates pass. Capture memory peak, loading time and first 30 seconds after loading. Stop before paging collapse; an aborted run is stability evidence, not an FPS sample.
5. For each necessary comparison, use the same executable with a mode toggle where possible, scene/camera/actions, patches, display/present mode and matched cache conditions. Compare reference behavior, legacy DRS, presenter-only reduction and complete 1080p distinctly. Test only the pair needed to resolve the current change.

Record guest flips and host presents separately; repeated frames are not unique rendered frames. Report average, p50/p95/p99, slowest-1%-mean FPS, count/duration of >50 and >100 ms gameplay stalls, foreground compilation, CPU critical-path time, GPU pass time, allocation peaks, transfer bytes and repeated recreation. Loading and heavy-profiler intervals are separate.

1% low = 1000 / mean milliseconds of the slowest 1% of eligible frames. Native mode should reduce eligible screen storage and measured GPU/copy work without a visual or p99 regression. Do not require an invented universal FPS gain when a scene remains CPU-bound; report that blocker and pursue S3.

The broader smooth-play milestone remains roughly 40 FPS with p95 ≤27.5 ms, p99 ≤30 ms, 1% low ≥35 and no repeatable action stall >50 ms on the covered route. These are uncapped engineering targets, not delivered claims. Native resolution and smooth gameplay are separate gates, both required for the final objective.

### Native 1080p acceptance checklist

- Main scene and final game composition verified at 1920×1080, including moving effects/history.
- No unreported 4K screen reconstruction/composition or presenter intermediate remains; unrelated high-resolution assets/shadows are enumerated separately.
- Game-registered dimensions and physical host dimensions are reported truthfully.
- No missing draws, effects, UI, updates, saves, audio behavior or guest completion events.
- Known shader reuse works after restart; new compilation is counted and bounded.
- Transition/revisit remains within resource limits or fails gracefully without data loss.
- 720p window tests are identified as such; 1:1 1080p display verification is a separate appropriately sized drawable check.
- Exact mode can be disabled with a direct rollback; no installed game file modification is needed.

## 9. Execution order and work-package boundaries

1. **Mapping package:** refresh HEAD/deployment; finish R0 and collect title allocation call paths for R1. Review existing S12/S13 and ASTRO resolution logs. Output is evidence and a precise intervention point.
2. **Early package:** implement R2 only if its measured memory/copy cost justifies it. In parallel, finish the one coherent R1 negotiation check and begin S1 cache/worker safeguards. R2 is not a reason to stop at downsampling.
3. **ASTRO native package:** implement R3 with exact profile gating, fixed scene/composition sizes and accurate clock behavior. Prove complete dependency coverage before deployment. If R3 is infeasible, document the missing path and build the smallest closed R4 group.
4. **Shared performance package:** continue M1/M2 and S3 against both games. The Demon's Souls tutorial CPU limit makes this work concurrent with resolution discovery. Review S2 candidates using measured shader costs.
5. **Demon's Souls native package:** identify and implement its own game profile or qualified R4 group. Reuse renderer infrastructure, not ASTRO assumptions.
6. **Integration package:** bound memory peaks, preserve cache compatibility, finish settings/status migration, run the focused transition/revisit gates, then stage a release candidate with rollback.
7. **Follow-on:** exact 1440p through the verified mechanism; broader title coverage only with evidence. Upscaling from below 1080p remains a separate future feature if requested.

R0/R1/R2 and labels are bounded investigations or small changes. R3 is title reverse engineering with uncertain duration. R4 is a multi-subsystem project, likely multiple iterations over weeks rather than a one-line fix. Shader cache work, coherence and per-draw efficiency each need coherent packages. Do not commit to a delivery date before R0/R3 identify feasibility.

Each package records symptom, source mechanism, invariant, predicted observable effect, rejection condition, tests and rollback. Build once per coherent package. Do not bundle a resolution override, new codegen, new alias semantics and threading rewrite into one unreviewable change.

## 10. Files to begin with

Paths below are relative to `C:\Users\himav\Desktop\kyty ps5-src` and were inspected or located during this plan:

- `src/main.cpp`: current INI parsing and startup configuration.
- `src/graphics/presentation/videoOut.cpp`: output reports, registration, attribute updates and flip contracts.
- `src/graphics/presentation/window/swapchain.cpp`: prepared frame pool, copy, composition and actual swapchain extent.
- `src/graphics/host_gpu/renderer/resolutionControl.{h,cpp}` and `sync.cpp`: current DRS clock behavior; replace or isolate it for native profiles.
- `src/graphics/host_gpu/renderer/colorRenderTarget.cpp`, `renderDraw.cpp`, `renderCompute.cpp`: attachment dimensions, raster state, direct/indirect execution.
- `src/graphics/host_gpu/renderer/image/imageInfo.h`, `image.cpp`, `tiler.cpp`, `cache/textureCache.cpp`, `cache/bufferCache.cpp`: logical layout, backing allocation, alias conversion and residency.
- `src/graphics/shader/recompiler/backend/spirv/spirvEmitterImage.cpp`, `spirvEmitterFlow.cpp`, `spirvEmitterModule.cpp`: image addressing/query and coordinate semantics for any reviewed R4 variant.
- `src/graphics/shader/recompiler/ir/passes/SrtWalker.{h,cpp}`; `src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp`, `descriptors.cpp`, `shaderPrecompile.{h,cpp}`, `pipelinePrewarm.{h,cpp}`: per-draw CPU preparation and cache/preparation architecture.

Existing evidence is under `Performance Experiments`: X23-res1, X28, X39/X43, X49–X52, E39/E40, and DS-demons-souls/S12–S13. Confirm matching build/cache/scene metadata before reusing a number. Do not manufacture missing recordings.

## 11. Handoff checkpoint for this plan

An isolated research workspace now exists at `Performance Experiments\NATIVE-1080P-ISOLATED`; its source is a separate worktree on `codex/native-1080p-research`, based on `ef996b5`. The active `kyty ps5-src` tree remains at `ef996b5` with its two pre-existing texture-cache edits; neither was touched. `Fixed Build` and `Optimized Build` were not modified.

- Baseline and resolution-trace executables are preserved in the isolated `builds` directory. The latest candidate, `resolution-trace-attrchange-ef996b5`, has SHA-256 `85dff0ca6dc45f5d578e75425391a742b164a97f6dc5ec14360e57a0a7a095c2`; it logs later VideoOut buffer-attribute extent changes as well as output hints and initial registration. It is not deployed.
- Source proves that `VideoOutGetOutputStatus` is only a hint; registered guest dimensions independently define tiled image layout. ASTRO X23-res1 and X47 both reported the 1080p hint while registering 3840×2160 buffers. The DRS preference observed 1920/2432/3328/3840 scene-target widths and clock scale up to 8.0, so it is not a reliable native-output mode.
- The presenter copies the resolved guest source image without scaling, then the host swapchain blits that image to the drawable extent. A 1280×720 test window and a 1080p status value therefore do not prove native 1080p rendering or output.
- X47 dune evidence is CPU critical-path heavy: 98.7% of GPU-thread samples were in PM4 handling, 63.0% in `DrawAuto`, and 43.7% in `GetGraphicsPrograms`; warm prewarm had 605/605 pipeline hits. The physical GPU busy percentage remains unmeasured. A separate selected 20 s window in X47 is 21.7 FPS (p50 49.7 ms, p99 70.8 ms, 1% low 13.3 FPS); other selected windows differ, so treat them as route-specific, not an A/B.
- Current available RAM is about 9 GiB, below the established 10.6 GiB game-test gate; no emulator is running. The latest Demon's Souls S16 test was stopped by its PID-owned watchdog as free RAM reached 315 MiB while its working set exceeded 11 GiB. Do not launch another game run until the test gate is met.
- The attribute-change trace source compiled successfully in the isolated Release build. A first fresh CMake setup fetched the pinned FFmpeg package into the isolated build cache; the package is local there now. No more downloads are needed for this candidate. Build outputs remain under the isolated workspace.

**Exact next action:** when the RAM gate is met and no user emulator is active, run the isolated candidate in its copied-save 1280×720 window, uncapped, only through the initial VideoOut negotiation. Capture title/version, output hint, register extent, and any later attribute-change trace. Then stop and inspect whether ASTRO changes guest presentation extent after startup. Use a separate short dune route with PID-safe GPU-engine/timestamp instrumentation only if a route-matched CPU/GPU attribution is still needed. Do not call the setting native 1080p or claim the dune GPU is idle until those measurements establish it.

## Current execution checkpoint — 2026-09-28

- The isolated VideoOut trace proves that the host 1280x720 window and the game output are separate: ASTRO receives the 1080p mode hint but registers 2x 3840x2160 buffers, and the presenter copies 3840x2160 to the 1280x720 drawable. Native 1080p is not achieved.
- The legacy DRS feedback moved its clock scale up to 8 while selected target widths reached 3840. A reversed-direction isolated experiment reduced scale to 0.58–0.69 in a short run, but selected widths still included 3840. Keep this candidate unverified; investigate whether the controller is counting post-process/final output targets before any deployment.
- Two boot-only runs grew working set past 5.6 GiB in under 30 seconds even with replay, pipeline prewarm and validation disabled. Both were gracefully closed by their exact test PIDs with RAM above 4 GiB; free RAM recovered. Do not launch the dune or transition route until available RAM is around 10.6 GiB.
- Existing dune evidence remains CPU-command-preparation heavy at 18.8–21.7 FPS in selected X47 windows, with 70–72 ms p99. The physical GPU busy percentage remains unmeasured; a matched, short GPU-timestamp capture is still needed.
- Next: inspect the DRS target histogram and guest-clock consumers; then, when RAM permits, capture one normal bounded dune interval with no FPS cap and report CPU critical path, GPU timestamps, unique presented frames and memory. Continue the native 1080p scene/composition map separately from VideoOut mode reporting.

### Safety correction — 2026-09-28
- The width histogram is now collected in isolated candidate `resolution-observe-no-steer-569d146` (source commit `569d146`). Its bind-count winner changed across 1920/2432/3328/3840 during startup; the prior steering heuristic therefore has no validated primary-scene input.
- The isolated candidate leaves reference-clock values unchanged by default; `KYTY_LEGACY_DRS_STEERING=1` is an explicitly experimental opt-in. This corrects the earlier “verified profile” wording. It is not deployed and does not make native 1080p work.
- A 1080p status hint still coexists with two guest 3840x2160 display buffers. The no-steer check ended on the opening shader-loading screen, before gameplay. One other boot was stopped by the owned-PID watchdog at the 3 GiB RAM floor. Current ~9.7 GiB free remains below the 10.6 GiB dune-route gate.
- Next native-resolution step is to identify the game's primary scene target and final composition by resource identity/lifetime, then follow its resolution decision inputs. Do not use the all-target width winner or reported output enum as proof of native rendering size.

### Research detail — reference-clock scope
Source search found `ResolutionControl::Adjust` is reached through the renderer's `Sync::ReadReferenceClock`, which supplies GPU guest writes from COPY_DATA reference-clock and EVENT_WRITE timestamp paths. These are guest-visible values and may feed more than one title subsystem. Keep steering opt-in until ASTRO's exact DRS query/write/read loop and primary render target are traced. The width-only observer currently aggregates every screen-shaped color-target bind; it does not distinguish scene, history, UI, post-process or output surfaces.

### Upstream shader/fidelity leads checked — 2026-09-28
The public KytyPS5 shader replay PR #718 is still open; its branch provides recorded shader replay, which is separate from the local Vulkan pipeline prewarm path. A reported precompile crash was later retracted after a clean configure/build removed stale cross-branch objects. Preserve clean build-tree discipline and review current PR state before porting further. PR #558 is also still open; it contains Astro SRT/ray-tracing work but explicitly leaves RT triangle leaves as misses and reports low title-screen FPS. Treat it as a correctness research lead, not a drop-in fidelity/performance patch. Issue #204 concerns game version 01.018.000, not this installed 01.007.000.

### Target identity instrumentation — 2026-09-28
- Isolated source commit `fc569fa` adds opt-in, fixed-memory attribution of screen-shaped render-target identities (guest address/range, extent, format, mip/layer, samples and tiling) across eight-frame windows. This replaces the width-only histogram as the next evidence source, but does not itself identify the primary scene target.
- Clean candidate `NATIVE-1080P-ISOLATED\\builds\\resolution-target-identity-fc569fa`, SHA-256 `64B6098E2B63BA454895E811C900C24492D0906600078CC782441F9397185DA6`; build passed. No runtime validation of this new logger yet. Clock steering stays off by default.
- Before any resolution change, correlate these resource identities to producer/consumer passes and VideoOut flips. Current free RAM (~9.7 GiB) remains below the 10.6 GiB gameplay-route gate.

### Scanout-range tracing and corrected setting semantics — 2026-09-28
- Latest boot-only runtime observation remained in the opening cave cinematic and did not reach the menu. One brief titlebar value near 45 FPS is not gameplay performance. ASTRO returned the 1080p output hint under a 1280x720 host context, but registered two guest display buffers at 3840x2160. Presenter input/backing/presenter remained 3840x2160 and host drawable 1280x720. This did not verify native 1080p rendering or 1080p host output. The histogram also saw 1920x1080, 2432x1368 and 3328x1872 targets; no primary-scene classification exists yet.
- Offline aggregation of top-six-per-window rows counted 3840x2160 in 135 windows/8,671 reported binds; 3328x1872 in 50/2,628; 2432x1368 in 16/6,538; and 1920x1080 in 32/2,407. These counts are truncated lower bounds. Addresses `0x0520440000` (3840) and `0x05168c0000` (2432) are recurrent candidates, not assigned scene/output roles.
- Vulkan format 58 maps to A2R10G10B10 UNORM, matching ASTRO's registered VideoOut format policy. Two adjacent recurring 3840x2160 format-58 targets (`0x0507410000`, `0x05093f0000`, each range `0x1fe0000`, about 31.9 MiB) are likely the double-buffered scanout pair; exact registered/flip address matching remains pending. That pair is only about 63.8 MiB, so reducing it alone cannot resolve multi-GB VRAM pressure. Format 97 maps to 64-bit R16G16B16A16_SFLOAT targets across the DRS-like extents; likely scene/HDR intermediates, still unclassified.
- Source inspection confirms isolated `render_resolution=1080p` only sets the observer's target-width preference. With legacy reference-clock steering disabled, the adjustment returns the original guest-visible clock and does not actuate resolution. The host 1080p hint is not proof of native render or scanout.
- Added bounded, opt-in tracing to isolated `videoOut.cpp` for registered display-buffer addresses/ranges and the first 32 actual flip source ranges. Runtime confirms the two registered 3840x2160 buffers exactly match the two recurring format-58 targets, and the first 32 flips alternate across them. Candidate `builds\scanout-range-trace-9bfa43f-dirty`, SHA-256 `D03B78857EADA3B3DB240F19E971ABD32E4D3744809544C67C6B44F4A5B6CF07`; single-worker Release build and boot trace passed. No gameplay/performance validation; isolated launcher selects it.
- An earlier boot PID 18596 was watchdog-stopped at 2045 MiB available RAM and peaked at 6254 MiB working set. The latest scanout run started at 9046 MiB and was gracefully closed by exact PID 2580 at 3634 MiB after ~37 seconds; RAM recovered to 8.92 GiB. Its first watchdog spawn failed due to unquoted spaced paths; corrected quoting was smoke-checked on the exited PID. No dune/transition run or GPU-use measurement was obtained. Continue static copy/resolve mapping; once the 10.6 GiB gate is met, take a short sampler-only dune run and a separate Tracy attribution run.
- To prevent repeating the spawn issue, the isolated `drive.py launch` now starts the watchdog automatically with Python's argument-list API, default 3072 MiB floor, and per-run log. Syntax and stale-PID startup checks passed; no active source or emulator behavior changed.
