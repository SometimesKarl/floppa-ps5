#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {

// Evaluates DCC fast-clear metadata on the GPU. When guest shaders wrote the metadata, the CPU
// cannot read it without draining the queue; instead a compute pass writes one predicate per
// candidate clear code, which conditional rendering then uses to apply the matching clear.
class DccClearResolver {
public:
	static constexpr uint32_t MaxCodes = 5;

	struct Predicates {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0; // predicate k at offset + 4 * k
	};

	DccClearResolver(GraphicContext& graphics, CommandScheduler& scheduler);
	~DccClearResolver();
	KYTY_CLASS_NO_COPY(DccClearResolver);

	[[nodiscard]] bool Available() const noexcept { return m_pipeline != nullptr; }
	[[nodiscard]] uint64_t StorageAlignment() const noexcept;

	// Records the detection for one metadata slice of `size` bytes (a multiple of 4 KiB) at
	// metadata_offset. Every byte equal to codes[k] sets predicate k; a match also rewrites the
	// slice to 0xff. Orders itself after all earlier queue work and before the predicate reads.
	[[nodiscard]] Predicates Detect(vk::CommandBuffer command, vk::Buffer metadata,
	                                uint64_t metadata_offset, uint64_t size,
	                                std::span<const uint8_t> codes);

private:
	static constexpr uint64_t SlotSize  = 256;
	static constexpr uint32_t SlotCount = 256;

	GraphicContext&         m_graphics;
	Buffer                  m_results;
	uint32_t                m_next_slot       = 0;
	vk::DescriptorSetLayout m_desc_layout     = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_DCCCLEARRESOLVER_H_
