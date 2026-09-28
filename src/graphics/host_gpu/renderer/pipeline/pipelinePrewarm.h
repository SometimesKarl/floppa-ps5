#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARM_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARM_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace Libs::Graphics {
struct GraphicContext;
}

// Records every pipeline the title creates (its SPIR-V modules and complete create info) and
// creates them again before the next session draws, so a driver pipeline cache that no longer
// matches (a new driver, a cache file lost in a crash) is rebuilt on all cores at boot instead
// of one pipeline at a time in the middle of play. Records hold SPIR-V, not guest state: they
// replay without the guest and outlive emulator builds.
namespace Libs::Graphics::PipelinePrewarm {

// Loads the records of earlier sessions and appends new ones to `path`.
void Open(const std::filesystem::path& path);
void Close();

void RecordModule(vk::ShaderModule module, std::span<const uint32_t> spirv);
// A destroyed module's handle may be reused for different code.
void ForgetModule(vk::ShaderModule module);

void RecordGraphics(const vk::GraphicsPipelineCreateInfo&            info,
                    std::span<const vk::DescriptorSetLayoutBinding> bindings,
                    vk::DescriptorSetLayoutCreateFlags               set_flags,
                    const vk::PushConstantRange&                     push_range);
void RecordCompute(const vk::PipelineShaderStageCreateInfo&         stage,
                   std::span<const vk::DescriptorSetLayoutBinding> bindings,
                   vk::DescriptorSetLayoutCreateFlags               set_flags,
                   const vk::PushConstantRange&                     push_range);

struct ReplayStats {
	uint32_t total    = 0; // pipelines recorded by earlier sessions
	uint32_t cached   = 0; // already in the driver cache
	uint32_t compiled = 0; // compiled now
	uint32_t failed   = 0; // could not be rebuilt (missing module, driver error)
	double   seconds  = 0;
};

// Creates every recorded pipeline with `driver_cache` on `threads` threads and destroys it again;
// `progress(done, total)` is called from the calling thread about four times a second.
ReplayStats Replay(GraphicContext& graphics, vk::PipelineCache driver_cache, uint32_t threads,
                   const std::function<void(uint32_t, uint32_t)>& progress);

} // namespace Libs::Graphics::PipelinePrewarm

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARM_H_
