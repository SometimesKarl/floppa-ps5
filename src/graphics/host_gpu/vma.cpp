#include "graphics/host_gpu/vulkanCommon.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wunused-private-field"
#pragma clang diagnostic ignored "-Wunused-variable"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>

namespace Libs::Graphics {

bool GraphicContext::CreateAllocator() {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(instance == nullptr || physical_device == nullptr || device == nullptr ||
	        allocator != nullptr);

	VmaVulkanFunctions functions {};
	functions.vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr   = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

	VmaAllocatorCreateInfo info {};
	info.instance         = instance;
	info.physicalDevice   = physical_device;
	info.device           = device;
	info.pVulkanFunctions = &functions;
	info.vulkanApiVersion = VULKAN_TARGET_API_VERSION;
	info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if (memory_budget_ext_enabled) {
		info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	}

	const auto result = static_cast<vk::Result>(vmaCreateAllocator(&info, &allocator));
	if (result != vk::Result::eSuccess) {
		LOGF("vmaCreateAllocator failed: %s\n", vk::to_string(result).c_str());
		return false;
	}
	return true;
}

void GraphicContext::DestroyAllocator() {
	if (allocator == nullptr) {
		return;
	}
	vmaDestroyAllocator(allocator);
	allocator = nullptr;
}

void GraphicContext::LogMemoryBudget() const {
	if (allocator == nullptr || physical_device == nullptr) {
		return;
	}

	const auto& properties = GetPhysicalDeviceMemoryProperties();
	VmaBudget   budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	// printf, not LOGF: this runs right before an allocation failure ends the process, and
	// must reach the console log in every configuration.
	for (uint32_t i = 0; i < properties.memoryHeapCount; i++) {
		std::printf("VMA heap %u: usage=%" PRIu64 ", budget=%" PRIu64 ", allocation=%" PRIu64
		            ", blocks=%" PRIu64 "\n",
		            i, static_cast<uint64_t>(budgets[i].usage),
		            static_cast<uint64_t>(budgets[i].budget),
		            static_cast<uint64_t>(budgets[i].statistics.allocationBytes),
		            static_cast<uint64_t>(budgets[i].statistics.blockBytes));
	}
	std::fflush(stdout);
}

namespace {

struct KindCounters {
	std::array<std::atomic<int64_t>, VK_MAX_MEMORY_TYPES> bytes {};
	std::array<std::atomic<int64_t>, VK_MAX_MEMORY_TYPES> count {};
};

std::array<KindCounters, static_cast<size_t>(GraphicContext::AllocationKind::Count)> g_accounting;

} // namespace

void GraphicContext::AccountAllocation(AllocationKind kind, VmaAllocation allocation,
                                       bool added) const {
	if (allocator == nullptr || allocation == nullptr || kind >= AllocationKind::Count) {
		return;
	}
	VmaAllocationInfo info {};
	vmaGetAllocationInfo(allocator, allocation, &info);
	if (info.memoryType >= VK_MAX_MEMORY_TYPES) {
		return;
	}
	auto&         counters = g_accounting[static_cast<size_t>(kind)];
	const int64_t sign     = added ? 1 : -1;
	counters.bytes[info.memoryType].fetch_add(sign * static_cast<int64_t>(info.size),
	                                          std::memory_order_relaxed);
	counters.count[info.memoryType].fetch_add(sign, std::memory_order_relaxed);
}

uint64_t GraphicContext::AllocationSize(VmaAllocation allocation) const {
	if (allocator == nullptr || allocation == nullptr) {
		return 0;
	}
	VmaAllocationInfo info {};
	vmaGetAllocationInfo(allocator, allocation, &info);
	return info.size;
}

void GraphicContext::PrintMemoryStatistics() const {
	if (allocator == nullptr) {
		return;
	}
	const auto&        properties = GetPhysicalDeviceMemoryProperties();
	VmaTotalStatistics stats {};
	vmaCalculateStatistics(allocator, &stats);
	constexpr double MiB = 1024.0 * 1024.0;
	for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
		const auto& s = stats.memoryType[type].statistics;
		if (s.blockCount == 0) {
			continue;
		}
		const auto flags = properties.memoryTypes[type].propertyFlags;
		std::printf("Memory type %u (heap %u, %s%s%s): blocks=%u %.0f MiB, allocations=%u %.0f MiB\n",
		            type, properties.memoryTypes[type].heapIndex,
		            (flags & vk::MemoryPropertyFlagBits::eDeviceLocal) ? "device-local " : "",
		            (flags & vk::MemoryPropertyFlagBits::eHostVisible) ? "host-visible " : "",
		            (flags & vk::MemoryPropertyFlagBits::eHostCached) ? "host-cached" : "",
		            s.blockCount, static_cast<double>(s.blockBytes) / MiB, s.allocationCount,
		            static_cast<double>(s.allocationBytes) / MiB);
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	for (uint32_t heap = 0; heap < properties.memoryHeapCount; heap++) {
		std::printf("Memory heap %u (%s, %.0f MiB): usage=%.0f MiB budget=%.0f MiB (VMA blocks %.0f MiB)\n",
		            heap,
		            (properties.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal)
		                ? "device-local"
		                : "system",
		            static_cast<double>(properties.memoryHeaps[heap].size) / MiB,
		            static_cast<double>(budgets[heap].usage) / MiB,
		            static_cast<double>(budgets[heap].budget) / MiB,
		            static_cast<double>(budgets[heap].statistics.blockBytes) / MiB);
	}
	static constexpr const char* KindNames[] {"images", "buffers device-local", "buffers upload",
	                                          "buffers download", "buffers stream"};
	for (size_t kind = 0; kind < g_accounting.size(); kind++) {
		for (uint32_t type = 0; type < properties.memoryTypeCount; type++) {
			const auto count = g_accounting[kind].count[type].load(std::memory_order_relaxed);
			if (count == 0) {
				continue;
			}
			std::printf("Memory owner %s in type %u: %" PRId64 " allocations, %.0f MiB\n",
			            KindNames[kind], type, count,
			            static_cast<double>(g_accounting[kind].bytes[type].load(
			                std::memory_order_relaxed)) /
			                MiB);
		}
	}
	std::fflush(stdout);
}

uint64_t GraphicContext::GetDeviceMemoryUsage() const {
	if (!CanReportMemoryUsage() || allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t usage = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const bool device_local =
		    static_cast<bool>(physical_device_memory_properties.memoryHeaps[heap].flags &
		                      vk::MemoryHeapFlagBits::eDeviceLocal);
		if (!discrete || device_local) {
			usage += budgets[heap].usage;
		}
	}
	return usage;
}

uint64_t GraphicContext::GetTotalMemoryBudget() const {
	if (allocator == nullptr) {
		return 0;
	}
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
	vmaGetHeapBudgets(allocator, budgets);
	const bool discrete =
	    physical_device_properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
	uint64_t budget = 0;
	uint64_t local  = 0;
	uint64_t usage  = 0;
	for (uint32_t heap = 0; heap < physical_device_memory_properties.memoryHeapCount; heap++) {
		const auto& properties = physical_device_memory_properties.memoryHeaps[heap];
		const bool  device_local =
		    static_cast<bool>(properties.flags & vk::MemoryHeapFlagBits::eDeviceLocal);
		if (device_local) {
			local += properties.size;
		}
		if (!discrete || device_local) {
			budget += CanReportMemoryUsage() ? budgets[heap].budget : properties.size;
			usage += CanReportMemoryUsage() ? budgets[heap].usage : 0;
		}
	}
	if (discrete) {
		return budget - std::min<uint64_t>(budget / 8, 1024ull * 1024 * 1024);
	}
	constexpr uint64_t system_reserve = 8ull * 1024 * 1024 * 1024;
	const auto         available      = budget > usage ? budget - usage : uint64_t {0};
	return std::max(local, available > system_reserve ? available - system_reserve : uint64_t {0});
}

bool GraphicContext::CreateImage(const vk::ImageCreateInfo& image_info, VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image != nullptr || image.allocation != nullptr);

	VmaAllocationCreateInfo alloc_info {};
	alloc_info.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	vk::Image::CType native_image = VK_NULL_HANDLE;
	auto              result       = static_cast<vk::Result>(
	    vmaCreateImage(allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info),
	                   &alloc_info, &native_image, &image.allocation, nullptr));
	if (result != vk::Result::eSuccess) {
		// Video memory is exhausted even past the budget: place the image in system memory
		// (slower to sample) rather than end the game; the texture collector frees VRAM later.
		static std::atomic<uint32_t> fallback_count {0};
		if (fallback_count.fetch_add(1, std::memory_order_relaxed) < 8) {
			std::printf("Image: %ux%u format=%d does not fit video memory (%s); using system "
			            "memory\n",
			            image_info.extent.width, image_info.extent.height,
			            static_cast<int>(image_info.format), vk::to_string(result).c_str());
			LogMemoryBudget();
		}
		alloc_info.requiredFlags  = 0;
		alloc_info.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		native_image              = VK_NULL_HANDLE;
		result                    = static_cast<vk::Result>(vmaCreateImage(
		    allocator, static_cast<const vk::ImageCreateInfo::NativeType*>(image_info), &alloc_info,
		    &native_image, &image.allocation, nullptr));
	}
	image.image = native_image;
	if (result != vk::Result::eSuccess) {
		LogMemoryBudget();
		return false;
	}
	AccountAllocation(AllocationKind::Image, image.allocation, true);

	image.format     = image_info.format;
	image.image_type = image_info.imageType;
	image.extent     = image_info.extent;
	image.layers     = image_info.arrayLayers;
	image.mip_levels = image_info.mipLevels;
	image.samples    = static_cast<uint32_t>(image_info.samples);
	image.usage      = image_info.usage;
	image.flags      = image_info.flags;
	image.state      = {.layout = image_info.initialLayout};
	image.subresource_states.clear();

	return true;
}

void GraphicContext::DeleteImage(VulkanImage& image) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(allocator == nullptr || image.image == nullptr || image.allocation == nullptr);

	AccountAllocation(AllocationKind::Image, image.allocation, false);
	vmaDestroyImage(allocator, image.image, image.allocation);
	image.image      = nullptr;
	image.allocation = nullptr;
}

} // namespace Libs::Graphics
