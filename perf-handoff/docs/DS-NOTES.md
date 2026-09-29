# Demon's Souls (PPSA01342) on KytyPS5 — test log

Game: `S:\[SuperPSX]-Demons.Souls-PPSA01342-USA-PS5\...\PPSA01342-app0\eboot.bin`, no game patch.
Launcher: `tools\game_session.py` (same arguments as the ASTRO BOT launcher, `--game` swapped, validation off),
working directory `DS-demons-souls\cwd` (own saves and pipeline cache). Machine: RX 6650 XT, Adrenalin 32.0.21045.5002.

## Boot crash (fixed in 51c13d5)
- Symptom (user, Fixed Build 7855c98; S1 on 6f0d3f8; S2 on abc2850): ~20-140 s after launch, after ~19 compute
  shaders: `Unhandled host exception ... pc=...amdvlk64.dll+0x22241fc access=1 address=0x1` on a host thread.
  Same report upstream: KytyPS5 issue #821 (RX 6700 XT, Windows).
- 9e1bde3 adds a host backtrace to the fault handler. S2: thread `Thread_Gpu`; chain
  CpOpDispatchDirect -> RenderExecutor::DispatchDirect -> RebindImages -> TextureCache::FindTexture -> TouchImage
  -> TileManager::Detile -> TileManager::Record -> amdvlk64.dll (12 driver frames: pipeline compilation).
  Symbolized with llvm-symbolizer (VS BuildTools LLVM) against builds\abc2850-clangcl\kyty_emulator.pdb.
- d19d8af logs each lazily created tiler pipeline: the last one before the crash was slot 23 = family 4
  (standard64_3d, 64 KiB 3D swizzle), 8-byte elements, detile.
- Fix 51c13d5: the 3D tiler families (standard4_3d, standard64_3d, prt_3d) are also compiled with the element size as
  a compile-time constant (`-DFIXED_ELEMENT_BYTES`), and GetPipeline uses those modules. The AMD Windows driver's
  compiler crashed on the specialization-constant form. S4: slot 23 created fine; boot continued (CS 47+).

## Runs
| Run | Build | Result |
|---|---|---|
| S1 | 6f0d3f8 | crash at 140 s in amdvlk64.dll+0x22241fc (Thread_Gpu) |
| S2 | abc2850 | same crash at 20 s; backtrace captured |
| S3 | d19d8af | same crash; last tiler pipeline slot 23 (standard64_3d, 8 B) |
| S4 | 51c13d5 | past the tiler crash (VS 2, PS 8, CS 118); stopped at 71 s: "depth attachment feedback loop is not supported by the host" (renderDraw.cpp:536) |
| S5 | 23a0a3e content (dirty; folder renamed 23a0a3e-dirty-was-51c13d5-clangcl) | past the depth-feedback stop (VS 11, PS 30, CS 157); stopped at 182 s: EXIT_IF(!DownloadBufferMemory) in buffer GC (bufferCache.cpp:747) |
| S6 | 2d0ffce (GC skips buffers owned by an in-flight read-back) | reached character creation (user: ~2 FPS, CPU and RAM maxed, GPU idle; frame log 2.9 FPS avg, max frame 60 s); stopped at 306 s: "download exceeds 64 MiB staging buffer capacity" |
| S7 | bb021fa (+ PR #842 batched downloads, KYTY_MEMORY_STATS) | ran 1004 s, no crash, user closed; 1.7 FPS avg, per minute 0/3/12.5/5.3/0.9/1.2/0.1/0/0.1, longest frame gap 357 s; 150 VS / 244 PS / 635 CS compiled |
| S8, S9 | d1b9d87 (thread priority) | clean exit (code 0) ~10 s after "Title ID", after loading the 17.9 MB pipeline cache |
| S10 | bb021fa, same cwd | also a clean exit at 30 s: caused by the cwd state (saves last written 07:22 during S5/S6, or the cache), not by d1b9d87 |

Parked 2026-09-27 08:00 at the user's request (ASTRO BOT first). Next for DS: retry with a fresh cwd copy (keep the pipeline
cache, drop _SaveData), then the job-worker spin (guest loop at 0x90086e000; HLE yield/usleep back-off).

## Depth feedback loop (fallback in 23a0a3e)
- The game samples a depth target that the same draw writes. The emulator used VK_EXT_attachment_feedback_loop_layout
  plus VK_EXT_attachment_feedback_loop_dynamic_state and exited without them. This AMD Windows driver
  (vulkaninfo) has the layout extension (attachmentFeedbackLoopLayout = true) but not the dynamic-state one.
- 23a0a3e: warn once and use the GENERAL layout (the existing path for sampled read-only depth). Possible better fix:
  use the layout extension alone with VK_PIPELINE_CREATE_DEPTH_STENCIL_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT pipelines
  (needs the feedback aspects in the pipeline key, i.e. render targets resolved before the pipeline lookup).

## Performance (S7 sampling, character creation path)
- tools/thread_top.py: 13 guest threads "BPE JobWorkerThread CPU0..12" at 68-94% of a core each, Thread_Gpu 87%,
  total ~1090% of one core on a 12-thread CPU. RAM at that point: WS 4.4 GB, 3.8 GB free; VRAM 2.0 GB.
- tools/thread_sample.py (suspend/GetThreadContext IP sampling): ~76% of job-worker samples in two guest code pages
  (0x90086e000-0x90086ffff: a busy-wait loop in game code), ~15% in ntdll yield/delay syscalls.
- All emulator and guest threads ran at Normal priority, so the GPU command thread was time-sliced with 13 spinners.
  d1b9d87 raises Thread_Gpu, Thread_GpuPriority and the video-out present thread to ABOVE_NORMAL (they block, never
  spin). Next if still slow: find the guest spin loop's HLE exits (yield/usleep) and back them off; cap job-worker
  affinity so one physical core stays free for the emulator.
- Upstream issues: #821 (same boot crash, RX 6700 XT), #697/#656 (older builds reached menu/game), #855 (SRT crash).
