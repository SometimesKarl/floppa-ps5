#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_

#include "common/abi.h"
#include "graphics/host_gpu/renderer/cache/streamBuffer.h"

#include <cstdint>

namespace Libs::Graphics {

// Converts guest DRAW_INDEX_INDIRECT arguments that GPU work wrote into host mesh-draw commands
// on the GPU (see mesh_indirect_args.comp), clamping the instance count to the device limits.
class MeshIndirectBuilder {
public:
	struct Command {
		vk::Buffer buffer = nullptr;
		uint64_t   offset = 0;
	};

	MeshIndirectBuilder(GraphicContext& graphics, CommandScheduler& scheduler);
	~MeshIndirectBuilder();
	KYTY_CLASS_NO_COPY(MeshIndirectBuilder);

	[[nodiscard]] bool Available() const noexcept { return m_pipeline != nullptr; }

	// Largest instance count one command can launch with `groups` workgroups per instance.
	[[nodiscard]] uint32_t MaxInstances(uint32_t groups) const noexcept;

	// Records the conversion outside any render pass. Orders itself after all earlier queue work
	// (the argument writes and earlier reads of the reused slot) and before indirect reads.
	[[nodiscard]] Command Record(vk::CommandBuffer command, vk::Buffer args, uint64_t args_offset,
	                             uint32_t groups, uint32_t bound);

	// True once any conversion had to clamp an instance count. The flag reaches the CPU after
	// the GPU ran that conversion, so a few draws may have been clamped by then.
	[[nodiscard]] bool Overflowed();

private:
	static constexpr uint64_t SlotSize  = 16;
	static constexpr uint32_t SlotCount = 1024;

	GraphicContext&         m_graphics;
	Buffer                  m_commands;
	Buffer                  m_status;
	uint32_t                m_next_slot       = 0;
	bool                    m_overflowed      = false;
	vk::DescriptorSetLayout m_desc_layout     = nullptr;
	vk::PipelineLayout      m_pipeline_layout = nullptr;
	vk::Pipeline            m_pipeline        = nullptr;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_MESHINDIRECT_H_
