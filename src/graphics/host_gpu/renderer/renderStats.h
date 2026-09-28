#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <unordered_map>

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
// Guest flips submitted from the GPU command stream: the frame count render targets are dated by.
inline std::atomic<uint64_t> g_guest_frames {0};

inline void Count(std::atomic<uint64_t>& counter) {
	counter.fetch_add(1, std::memory_order_relaxed);
}

// Color render-target sizes bound by draws (key: width << 32 | height), with KYTY_GPU_STATS: the
// most used sizes show the game's internal render resolution.
inline std::mutex                             g_target_mutex;
inline std::unordered_map<uint64_t, uint64_t> g_target_sizes;

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_GPU_STATS") != nullptr;
	return enabled;
}

inline void CountTarget(uint32_t width, uint32_t height) {
	std::scoped_lock lock {g_target_mutex};
	g_target_sizes[(static_cast<uint64_t>(width) << 32u) | height]++;
}

// Where recorded rendering was ended, with KYTY_GPU_STATS: a source location (file, line) of an
// explicit EndRendering, or a reason (line 0) when the next draw needed other attachments.
inline std::mutex                                           g_break_mutex;
inline std::map<std::pair<const char*, uint32_t>, uint64_t> g_breaks;

inline void CountBreak(const char* where, uint32_t line) {
	std::scoped_lock lock {g_break_mutex};
	g_breaks[{where, line}]++;
}

} // namespace Libs::Graphics::RenderStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERSTATS_H_
