#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_

#include <atomic>
#include <cstdint>

// Counters of host GPU work structure, printed every 10 s with KYTY_GPU_STATS=1 (see
// RenderContext::RunGarbageCollector): how often rendering is broken up and synchronized.
namespace Libs::Graphics::RenderStats {

inline std::atomic<uint64_t> g_begin_rendering {0};   // vkCmdBeginRendering actually recorded
inline std::atomic<uint64_t> g_end_rendering {0};     // vkCmdEndRendering actually recorded
inline std::atomic<uint64_t> g_global_barriers {0};   // CommandProcessor::EmitGlobalBarrier
inline std::atomic<uint64_t> g_redundant_global {0};  // ... with no draw/dispatch since the last one
inline std::atomic<uint64_t> g_image_barriers {0};    // Image::Transit barrier batches
inline std::atomic<uint64_t> g_draws {0};             // guest draws recorded
inline std::atomic<uint64_t> g_dispatches {0};        // guest dispatches recorded

inline void Count(std::atomic<uint64_t>& counter) {
	counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace Libs::Graphics::RenderStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_
