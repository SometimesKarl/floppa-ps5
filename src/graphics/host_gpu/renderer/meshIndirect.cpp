#include "graphics/host_gpu/renderer/meshIndirect.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/mesh_indirect_args_spv.h"
#include "graphics/host_gpu/gpuProfiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Libs::Graphics {

namespace {

struct ConvertParams {
	uint32_t args_dword    = 0;
	uint32_t command_dword = 0;
	uint32_t groups        = 0;
	uint32_t bound         = 0;
	uint32_t max_instances = 0;
};

} // namespace

MeshIndirectBuilder::MeshIndirectBuilder(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics),
      m_commands(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                 vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eIndirectBuffer,
                 SlotSize * SlotCount),
      m_status(graphics, scheduler, MemoryUsage::Download, 0,
               vk::BufferUsageFlagBits::eStorageBuffer, 256) {
	if (!graphics.mesh_shader_enabled || m_status.Mapped().size() < sizeof(uint32_t)) {
		return;
	}
	SetVulkanObjectNameF(m_graphics.device, m_commands.Handle(), "Mesh Indirect Commands");
	std::memset(m_status.Mapped().data(), 0, sizeof(uint32_t));
	m_status.Flush(0, sizeof(uint32_t));

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_desc_layout),
	    "create mesh indirect descriptor layout");

	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                        sizeof(ConvertParams)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_desc_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                            &m_pipeline_layout),
	                     "create mesh indirect pipeline layout");

	const auto module = CompileSPV(MESH_INDIRECT_ARGS_SPV, m_graphics.device);
	vk::PipelineShaderStageCreateInfo stage {};
	stage.stage  = vk::ShaderStageFlagBits::eCompute;
	stage.module = module;
	stage.pName  = "main";
	vk::ComputePipelineCreateInfo pipeline_info {};
	pipeline_info.stage  = stage;
	pipeline_info.layout = m_pipeline_layout;
	const auto result =
	    m_graphics.device.createComputePipelines(nullptr, 1, &pipeline_info, nullptr, &m_pipeline);
	m_graphics.device.destroyShaderModule(module, nullptr);
	RequireVulkanSuccess(result, "create mesh indirect pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_pipeline, "Mesh Indirect Args");
}

MeshIndirectBuilder::~MeshIndirectBuilder() {
	if (m_pipeline != nullptr) {
		m_graphics.device.destroyPipeline(m_pipeline, nullptr);
	}
	if (m_pipeline_layout != nullptr) {
		m_graphics.device.destroyPipelineLayout(m_pipeline_layout, nullptr);
	}
	if (m_desc_layout != nullptr) {
		m_graphics.device.destroyDescriptorSetLayout(m_desc_layout, nullptr);
	}
}

uint32_t MeshIndirectBuilder::MaxInstances(uint32_t groups) const noexcept {
	const auto& limits = m_graphics.mesh_shader_properties;
	return std::min(limits.maxMeshWorkGroupCount[1],
	                limits.maxMeshWorkGroupTotalCount / std::max(groups, 1u));
}

MeshIndirectBuilder::Command MeshIndirectBuilder::Record(vk::CommandBuffer command, vk::Buffer args,
                                                         uint64_t args_offset, uint32_t groups,
                                                         uint32_t bound) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!Available() || args == nullptr || (args_offset & 3u) != 0 || groups == 0 ||
	        groups > m_graphics.mesh_shader_properties.maxMeshWorkGroupCount[0] ||
	        MaxInstances(groups) == 0);

	const uint64_t alignment   = std::max<uint64_t>(m_graphics.StorageMinAlignment(), 4);
	const uint64_t args_base   = args_offset - args_offset % alignment;
	const auto     slot_offset = SlotSize * m_next_slot;
	m_next_slot                = (m_next_slot + 1) % SlotCount;

	ConvertParams params {};
	params.args_dword    = static_cast<uint32_t>((args_offset - args_base) / 4);
	params.command_dword = 0;
	params.groups        = groups;
	params.bound         = bound;
	params.max_instances = MaxInstances(groups);

	// Waits for every earlier command: the guest work that wrote the arguments and the indirect
	// read of this slot SlotCount conversions ago.
	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                        0, nullptr);

	const vk::DescriptorBufferInfo infos[] {
	    {args, args_base, args_offset - args_base + 3 * sizeof(uint32_t)},
	    {m_commands.Handle(), slot_offset, SlotSize},
	    {m_status.Handle(), 0, sizeof(uint32_t)},
	};
	std::array<vk::WriteDescriptorSet, 3> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(params),
	                      &params);
	{
		KYTY_GPU_ZONE(command, "GPU mesh indirect args");
		command.dispatch(1, 1, 1);
	}

	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead | vk::AccessFlagBits::eHostRead;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eDrawIndirect |
	                            vk::PipelineStageFlagBits::eHost,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
	return {m_commands.Handle(), slot_offset};
}

bool MeshIndirectBuilder::Overflowed() {
	if (m_overflowed || !Available()) {
		return m_overflowed;
	}
	m_status.Invalidate(0, sizeof(uint32_t));
	uint32_t value = 0;
	std::memcpy(&value, m_status.Mapped().data(), sizeof(value));
	m_overflowed = value != 0;
	return m_overflowed;
}

} // namespace Libs::Graphics
