#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPROFILER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPROFILER_H_

#include "common/profiler.h"
#include "graphics/host_gpu/vulkanCommon.h" // IWYU pragma: keep

// The Vulkan loader is resolved at run time (VK_NO_PROTOTYPES), so Tracy loads its entry points.
#define TRACY_VK_USE_SYMBOL_TABLE
#include <tracy/TracyVulkan.hpp> // IWYU pragma: export

#include <optional>

namespace Libs::Graphics {

struct GraphicContext;

// GPU timestamps for Tracy, active only with --profile. Zones are recorded only on the thread
// that owns the renderer's command stream (the guest GPU thread), which also collects results,
// so query resets and writes stay ordered on one queue. A zone must not span a submit: wrap the
// individual vkCmd* calls, not CPU functions that may flush.
namespace GpuProfiler {

void              Initialize(GraphicContext& graphics);
void              Shutdown();
void              EnableZonesOnThisThread();
[[nodiscard]] tracy::VkCtx* ZoneContext() noexcept;
// Call right after vkBeginCommandBuffer (outside any render pass) on the zone thread.
void              Collect(vk::CommandBuffer command);

} // namespace GpuProfiler

} // namespace Libs::Graphics

#define KYTY_GPU_ZONE_IMPL(line, command, name)                                                  \
	static constexpr tracy::SourceLocationData KYTY_PROFILER_CONCAT(kyty_gpu_zone_loc_, line) {   \
	    name, TracyFunction, TracyFile, static_cast<uint32_t>(line), 0};                         \
	std::optional<tracy::VkCtxScope> KYTY_PROFILER_CONCAT(kyty_gpu_zone_, line);                 \
	if (auto* kyty_gpu_zone_ctx = ::Libs::Graphics::GpuProfiler::ZoneContext();                \
	    kyty_gpu_zone_ctx != nullptr) {                                                          \
		KYTY_PROFILER_CONCAT(kyty_gpu_zone_, line)                                               \
		    .emplace(kyty_gpu_zone_ctx, &KYTY_PROFILER_CONCAT(kyty_gpu_zone_loc_, line),         \
		             static_cast<VkCommandBuffer>(command), true);                              \
	}

// Times the GPU work of the vkCmd* calls recorded into `command` until the end of the scope.
#define KYTY_GPU_ZONE(command, name) KYTY_GPU_ZONE_IMPL(__LINE__, command, name)

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_GPUPROFILER_H_
