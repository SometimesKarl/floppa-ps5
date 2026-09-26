#include "graphics/host_gpu/gpuProfiler.h"

#include "common/assert.h"
#include "common/threads.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics::GpuProfiler {

namespace {

tracy::VkCtx*            g_context      = nullptr;
thread_local bool        g_zone_thread  = false;

} // namespace

void Initialize(GraphicContext& graphics) {
	if (!tracy::ProfilerAvailable() || g_context != nullptr) {
		return;
	}
	// Tracy calibrates with its own one-time submissions and waits for the queue to idle.
	vk::CommandPoolCreateInfo pool_info {};
	pool_info.flags            = vk::CommandPoolCreateFlagBits::eTransient;
	pool_info.queueFamilyIndex = graphics.queue_family;
	vk::CommandPool pool       = nullptr;
	if (graphics.device.createCommandPool(&pool_info, nullptr, &pool) != vk::Result::eSuccess) {
		return;
	}
	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = 1;
	vk::CommandBuffer command   = nullptr;
	if (graphics.device.allocateCommandBuffers(&allocate, &command) == vk::Result::eSuccess) {
		Common::LockGuard lock(graphics.queue_mutex);
		g_context = tracy::CreateVkContext(
		    graphics.instance, graphics.physical_device, graphics.device, graphics.queue, command,
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr, graphics.calibrated_timestamps_enabled);
		constexpr char name[] = "Host GPU queue";
		g_context->Name(name, sizeof(name) - 1);
	}
	graphics.device.destroyCommandPool(pool, nullptr);
}

void Shutdown() {
	if (g_context != nullptr) {
		tracy::DestroyVkContext(g_context);
		g_context = nullptr;
	}
}

void EnableZonesOnThisThread() {
	g_zone_thread = true;
}

tracy::VkCtx* ZoneContext() noexcept {
	return g_zone_thread ? g_context : nullptr;
}

void Collect(vk::CommandBuffer command) {
	if (g_zone_thread && g_context != nullptr) {
		g_context->Collect(static_cast<VkCommandBuffer>(command));
	}
}

} // namespace Libs::Graphics::GpuProfiler
