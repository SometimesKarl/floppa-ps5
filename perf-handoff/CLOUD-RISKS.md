# Cloud risks and gaps (unresolved; keep current)

Evidence labels: historical observation, source-supported mechanism, hypothesis, cloud-tested,
local-validation-pending, rejected experiment.

## Open correctness questions
| # | Risk | Label | Where | What would settle it |
|---|---|---|---|---|
| R1 | Two identical attachment-write transitions across separate render pass instances emit no barrier (`state == destination && !repeated_write`; attachment writes are not in `write_access`). Store of pass N and load/test of pass N+1 then rely on driver ordering. | source-supported mechanism, hazard unproven | image/image.cpp AppendBarriers | Vulkan sync validation (`VK_LAYER_KHRONOS_validation` with synchronization validation) on a short ASTRO/DS capture; or a lavapipe test with sync validation if the layer is available |
| R2 | 9c52585 removes only sampling/sampling repeats: in a writable-stencil depth layout a non-sampling draw after a sampling one still changes access (Read\|Write\|ShaderRead -> Read\|Write), so the barrier and pass break stay | source-supported mechanism | renderDraw.cpp AcquireRenderTargets | Recipe entry 1 (DS KYTY_GPU_STATS): if the 673/s label remains, alternation is the cause |
| R3 | S20 guest fault: red zone clobbered by host exception dispatch on the guest stack | hypothesis (strong) | runtimeLinker.cpp, redZonePatcher.cpp | Recipe entry 10 (`--redzone` run) |
| R4 | New CheckBufferRange (b2f8985) ends the process on an out-of-range tiler request; a false positive would stop a game that ran before | local-validation-pending | textureCache.cpp | Recipe entry 11 |
| R5 | Precompile records have no per-record checksum; a damaged (not torn) record is translated and the recompiler may EXIT on garbage GCN | source-supported mechanism | shaderPrecompile.cpp Load | Only act if a boot stops right after "compiling recorded shaders"; fix = checksum + format version bump (discards recorded sets once) |

## Kept off / not implemented on purpose
- Making `--redzone` the default (patches guest code for every title) until R3 is confirmed.
- Folding transfer clears into the next pass's load op (reorders commands; needs the W4 hazard model).
- Deferring global barriers from RELEASE_MEM / EVENT_WRITE (guest-visible completion order).
- Extended multi-band uploads stay behind KYTY_PARTIAL_UPLOADS_EXT=1 (S17 freeze suspect).

## Environment limits
- No Windows toolchain here: clang-cl build and all Windows-only paths (fault handler backtrace,
  VirtualUnlock trim, red zone patcher, low-memory guard) are compile-unverified on Windows.
- No AMD GPU: lavapipe results say nothing about AMD driver behaviour or timing.
