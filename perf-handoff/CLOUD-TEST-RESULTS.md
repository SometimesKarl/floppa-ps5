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

## ctest (`ctest --test-dir _Build/linux-no-qt --timeout 600 -j1`), at e2c55da
| Result | Count | Tests |
|---|---|---|
| Passed | 34 | emulator_user_name_cli, emulator_user_name_too_long, emulator_present_mode_cli, emulator_present_mode_invalid, ime_dialog, shader_cfg, scalar_provenance, image_page_table, memory_tracker, buffer_download_batch, page_manager, archive_file, avplayer_file, bit_array, pipeline_prewarm_format (new), low_memory_guard (new), lru_cache, mesh_dispatch, shader_vertex_metadata, resource_materialization, resource_tracking, event_queue_lifetime, sync_on_address, audio_out2_port, ngs2_sampler, pad_haptics, save_data_memory, ces, save_data_dialog, http_uri_parse, graphics_draw_offsets, virtual_memory_allocation, guest_red_zone_patcher, texture_cache_depth_readback |
| Blocked by environment (not passed) | 15 | shader_recompiler_compute, shader_recompiler_alignbyte, graphics_pipeline_rasterization, command_scheduler_timeline, stream_buffer_ring, gpu_command_lane, pm4_context_state, gpu_tiler, texture_cache_layered_image, host_image_allocation, texture_cache_image_views, texture_cache_storage_sampled, compute_meta_clear_classification, buffer_cache_dirty_gc, color_render_target_1d: all stop at "VulkanHarness failed at dispatch: no Vulkan graphics+compute device with fragment barycentrics and 64-bit LDS atomics" |
| Blocked by environment (not passed) | 1 | kernel_file_system: needs a Vulkan window device. With `SDL_VIDEO_DRIVER=offscreen XDG_RUNTIME_DIR=<dir>` SDL starts but "Could not find suitable device" (same barycentrics requirement) |
| Failed (code) | 0 | shader_cfg failed before e2c55da (stale mesh-prolog test, pre-existing; fixed in the test) |

Consequence: the GPU-side changes of this session (b2f8985 texture cache, f1061f8 prewarm parser is
CPU-only and tested) and the Vulkan sync-validation idea for risk R1 cannot be exercised here.

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
