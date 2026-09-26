#include "graphics/host_gpu/renderer/cache/dccClearResolver.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "gpu_tiler_shaders/dcc_clear_detect_spv.h"
#include "graphics/host_gpu/gpuProfiler.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics {

namespace {

struct DetectParams {
	uint32_t                                         dword_count = 0;
	uint32_t                                         code_count  = 0;
	std::array<uint32_t, DccClearResolver::MaxCodes> patterns {};
};

} // namespace

DccClearResolver::DccClearResolver(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_graphics(graphics),
      m_results(graphics, scheduler, MemoryUsage::DeviceLocal, 0,
                vk::BufferUsageFlagBits::eStorageBuffer |
                    vk::BufferUsageFlagBits::eConditionalRenderingEXT,
                SlotSize * SlotCount) {
	if (!graphics.conditional_rendering_enabled) {
		return;
	}
	SetVulkanObjectNameF(m_graphics.device, m_results.Handle(), "DCC Clear Predicates");

	const vk::DescriptorSetLayoutBinding bindings[] {
	    {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	    {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
	};
	vk::DescriptorSetLayoutCreateInfo layout_info {};
	layout_info.flags        = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR;
	layout_info.bindingCount = std::size(bindings);
	layout_info.pBindings    = bindings;
	RequireVulkanSuccess(
	    m_graphics.device.createDescriptorSetLayout(&layout_info, nullptr, &m_desc_layout),
	    "create DCC clear descriptor layout");

	const vk::PushConstantRange push_range {vk::ShaderStageFlagBits::eCompute, 0,
	                                        sizeof(DetectParams)};
	vk::PipelineLayoutCreateInfo pipeline_layout_info {};
	pipeline_layout_info.setLayoutCount         = 1;
	pipeline_layout_info.pSetLayouts            = &m_desc_layout;
	pipeline_layout_info.pushConstantRangeCount = 1;
	pipeline_layout_info.pPushConstantRanges    = &push_range;
	RequireVulkanSuccess(m_graphics.device.createPipelineLayout(&pipeline_layout_info, nullptr,
	                                                            &m_pipeline_layout),
	                     "create DCC clear pipeline layout");

	const auto module = CompileSPV(DCC_CLEAR_DETECT_SPV, m_graphics.device);
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
	RequireVulkanSuccess(result, "create DCC clear pipeline");
	SetVulkanObjectNameF(m_graphics.device, m_pipeline, "DCC Clear Detect");
}

DccClearResolver::~DccClearResolver() {
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

uint64_t DccClearResolver::StorageAlignment() const noexcept {
	return std::max<uint64_t>(
	    m_graphics.physical_device_properties.limits.minStorageBufferOffsetAlignment, 4);
}

DccClearResolver::Predicates DccClearResolver::Detect(vk::CommandBuffer command, vk::Buffer metadata,
                                                      uint64_t metadata_offset, uint64_t size,
                                                      std::span<const uint8_t> codes) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(!Available() || codes.empty() || codes.size() > MaxCodes || size == 0 ||
	        size % 4 != 0 || size / 4 > UINT32_MAX || metadata_offset % StorageAlignment() != 0);

	DetectParams params {};
	params.dword_count = static_cast<uint32_t>(size / 4);
	params.code_count  = static_cast<uint32_t>(codes.size());
	for (size_t k = 0; k < codes.size(); k++) {
		params.patterns[k] = 0x01010101u * codes[k];
	}
	// The queue orders reuse: this barrier waits for every earlier command, including the
	// conditional-rendering reads of a slot handed out SlotCount calls ago.
	const auto slot_offset = SlotSize * m_next_slot;
	m_next_slot            = (m_next_slot + 1) % SlotCount;

	vk::MemoryBarrier before {};
	before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                        vk::PipelineStageFlagBits::eComputeShader, {}, 1, &before, 0, nullptr,
	                        0, nullptr);

	const vk::DescriptorBufferInfo infos[] {
	    {metadata, metadata_offset, size},
	    {m_results.Handle(), slot_offset, SlotSize},
	};
	std::array<vk::WriteDescriptorSet, 2> writes {};
	for (uint32_t index = 0; index < writes.size(); ++index) {
		writes[index].dstBinding      = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType  = vk::DescriptorType::eStorageBuffer;
		writes[index].pBufferInfo     = &infos[index];
	}
	command.bindPipeline(vk::PipelineBindPoint::eCompute, m_pipeline);
	command.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, m_pipeline_layout, 0, writes);
	command.pushConstants(m_pipeline_layout, vk::ShaderStageFlagBits::eCompute, 0,
	                      sizeof(params), &params);
	{
		KYTY_GPU_ZONE(command, "GPU DCC clear detect");
		command.dispatch(1, 1, 1);
	}

	vk::MemoryBarrier after {};
	after.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
	after.dstAccessMask = vk::AccessFlagBits::eConditionalRenderingReadEXT |
	                      vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
	                        vk::PipelineStageFlagBits::eConditionalRenderingEXT |
	                            vk::PipelineStageFlagBits::eAllCommands,
	                        {}, 1, &after, 0, nullptr, 0, nullptr);
	return {m_results.Handle(), slot_offset};
}

} // namespace Libs::Graphics
