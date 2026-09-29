# Cloud test results

## Environment (2026-09-29)
- Ubuntu 24.04 container, 4 cores, 15 GB RAM; clang 18 (Release, `-O3 -DNDEBUG`, `-fno-exceptions`).
- Vulkan: Mesa 25.2.8 lavapipe (llvmpipe, LLVM 20). Validation layer 1.3.275 installed.
- lavapipe here lacks `VK_KHR_fragment_shader_barycentric` (has 64-bit shared/buffer atomics); the
  emulator's device selection and the Vulkan test harness both require barycentrics. Newer Mesa is not
  installable (PPA host blocked by the network policy).

## Build
- `cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests -j3 -- -k 0`: exit 0 at d9d0f34
  (before e2c55da, which changes a test only). `kyty_emulator` links on Linux.

## ctest (`XDG_RUNTIME_DIR=<dir> ctest --test-dir _Build/linux-no-qt --output-on-failure --timeout 900 -j1`)
History: at e2c55da 34 passed / 16 environment-blocked (harness required fragment barycentrics and
image view min LOD; lavapipe has neither). ec6c6f7 makes both optional in the harness (still enabled
where supported), so the GPU lane runs on lavapipe. Results at ec6c6f7:

| Result | Count | Tests |
|---|---|---|
| Passed | 47 | all CPU tests (34, see e2c55da list) plus shader_recompiler_alignbyte, graphics_pipeline_rasterization, command_scheduler_timeline, stream_buffer_ring, gpu_command_lane, pm4_context_state, gpu_tiler (after 3b4c5a0), texture_cache_layered_image, host_image_allocation, texture_cache_image_views, texture_cache_storage_sampled, buffer_cache_dirty_gc, color_render_target_1d |
| Blocked by environment | 1 | kernel_file_system: needs the emulator's own Vulkan window device, which requires barycentrics |
| Failed, pre-existing | 2 | shader_recompiler_compute ("UnifiedTextureCacheFlow ... registered compatible backing did not reuse one ImageId") and compute_meta_clear_classification ("aliased clear did not reuse its existing UNORM allocation", after "recreating a format 91 image ... for other view formats"). Both fail identically with textureCache.cpp from 9ceebc4, so not caused by this session. Cause: the fork's view-format list for color targets (e464a1a) recreates an image once when viewed in another format; the upstream tests expect the old ImageId (CLOUD-RISKS R7) |
| Timing-sensitive | (1) | gpu_command_lane "32-bit release boundary": failed 1 of 13 runs, only inside the full batch; a label-only release submits when > 1 ms passed since the last flush (bounded coalescing, by design). Interrupt releases flush at the latest at the end of the guest submission slice (graphicsRun.cpp) |

Fixed along the way: shader_cfg (stale test, e2c55da); gpu_tiler (PRT shader, 3b4c5a0).
Pitfall: archive_file fails ("decode extended-length archive names") when ZArchive is rebuilt without
3rdparty/patches/zarchive-reader.patch; see the reconfigure note in CLOUD-STATE.md.

## Tests added this session and how they were checked
| Test | Checks | Extra verification |
|---|---|---|
| pipeline_prewarm_format | 676 | ASan+UBSan clean; mutation: removing each new check fails its case |
| low_memory_guard | 22 | ASan+UBSan clean; mutation: restoring "0 MB = unknown" fails |
| resource_materialization (3 new cases) | 2000 seeded differential walks, seed 0x5eed1234 | ASan+UBSan+LSan clean; mutations (no generation bump, conditions never decided) caught |
| shader_cfg (mesh prolog case updated) | existing checks + indirect-argument predicate | passes |

## How to reproduce
```sh
git -C _Build/linux-no-qt/_deps/zarchive_source-src checkout -- .   # before any reconfigure
cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DKYTY_BUILD_LAUNCHER=OFF
cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests -j3 -- -k 0
ctest --test-dir _Build/linux-no-qt --output-on-failure --timeout 600 -j1
```

## Windows-only code: MinGW syntax lane
No MSVC SDK or clang-cl here. Windows branches are syntax-checked with
`clang++ --target=x86_64-w64-mingw32 -std=c++20 -fsyntax-only -D_WIN32_WINNT=0x0A00` using a copy of
`_Build/linux-no-qt/cmake_config.h` with `KYTY_PLATFORM KYTY_PLATFORM_WINDOWS` first on the include path
(package mingw-w64). Checked clean for new code: systemInfo.cpp, main.cpp, memory.cpp (RAM attribution).
Existing code reports MinGW-header gaps (VirtualAlloc2, MapViewOfFile3, GetThreadDescription); those
are not errors under the MSVC SDK. This is not a substitute for the owner's clang-cl build.
