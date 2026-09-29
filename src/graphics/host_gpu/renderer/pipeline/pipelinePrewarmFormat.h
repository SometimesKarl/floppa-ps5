#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>
#include <xxhash.h>

// Record format of the pipeline prewarm file: the parts that need no Vulkan device (unit-tested in
// tests/PipelinePrewarmFormatTests.cpp). Writing records and replaying them live in
// pipelinePrewarm.cpp.
namespace Libs::Graphics::PipelinePrewarm {

constexpr uint32_t SpirvMagic      = 0x07230203u;
constexpr uint32_t SpirvHeaderWords = 5;

// A module record is { u64 hash, SPIR-V words } with hash = XXH3 of the words as recorded.
// Records are handed to the driver's compiler at boot without the guest, so a record whose words
// no longer match its hash (a damaged file) is rejected here rather than compiled: this driver
// has crashed inside its compiler on SPIR-V it did not expect (DS-NOTES, tiler slot 23).
inline bool DecodeModuleRecord(std::span<const uint8_t> payload, uint64_t& hash,
                               std::vector<uint32_t>& words) {
	if (payload.size() < sizeof(uint64_t) + SpirvHeaderWords * sizeof(uint32_t) ||
	    (payload.size() - sizeof(uint64_t)) % sizeof(uint32_t) != 0) {
		return false;
	}
	std::memcpy(&hash, payload.data(), sizeof(hash));
	words.resize((payload.size() - sizeof(uint64_t)) / sizeof(uint32_t));
	std::memcpy(words.data(), payload.data() + sizeof(uint64_t), words.size() * sizeof(uint32_t));
	return words[0] == SpirvMagic && XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)) == hash;
}

class Writer {
public:
	template <typename T>
	void Put(const T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
		data.insert(data.end(), bytes, bytes + sizeof(T));
	}
	template <typename T>
	void PutArray(const T* values, uint32_t count) {
		Put(count);
		for (uint32_t i = 0; i < count; i++) {
			Put(values[i]);
		}
	}
	std::vector<uint8_t> data;
};

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes): m_bytes(bytes) {}
	template <typename T>
	bool Get(T& value) {
		static_assert(std::is_trivially_copyable_v<T>);
		if (m_offset + sizeof(T) > m_bytes.size()) {
			m_ok = false;
			return false;
		}
		std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
		m_offset += sizeof(T);
		return true;
	}
	template <typename T>
	bool GetArray(std::vector<T>& values) {
		uint32_t count = 0;
		if (!Get(count) || count > 4096) {
			m_ok = false;
			return false;
		}
		values.resize(count);
		for (auto& value: values) {
			Get(value);
		}
		return m_ok;
	}
	[[nodiscard]] bool Ok() const { return m_ok && m_offset == m_bytes.size(); }
	void               Fail() { m_ok = false; }

private:
	std::span<const uint8_t> m_bytes;
	size_t                   m_offset = 0;
	bool                     m_ok     = true;
};

struct Layout {
	uint32_t                                     set_flags = 0;
	std::vector<vk::DescriptorSetLayoutBinding>  bindings;
	vk::PushConstantRange                        push {};
};

inline bool GetLayout(Reader& r, Layout& layout) {
	uint32_t count = 0;
	r.Get(layout.set_flags);
	if (!r.Get(count) || count > 1024) {
		return false;
	}
	layout.bindings.resize(count);
	for (auto& binding: layout.bindings) {
		uint32_t type = 0, stages = 0;
		r.Get(binding.binding);
		r.Get(type);
		r.Get(binding.descriptorCount);
		r.Get(stages);
		binding.descriptorType = static_cast<vk::DescriptorType>(type);
		binding.stageFlags     = vk::ShaderStageFlags(stages);
	}
	uint32_t push_stages = 0;
	r.Get(push_stages);
	r.Get(layout.push.offset);
	r.Get(layout.push.size);
	layout.push.stageFlags = vk::ShaderStageFlags(push_stages);
	return true;
}

// Everything vkCreateGraphicsPipelines reads, rebuilt from a record.
struct GraphicsReplay {
	std::vector<uint64_t>                                        module_hashes;
	std::vector<vk::PipelineShaderStageCreateInfo>               stages;
	std::vector<vk::VertexInputBindingDescription>               vertex_bindings;
	std::vector<vk::VertexInputAttributeDescription>             vertex_attributes;
	vk::PipelineVertexInputStateCreateInfo                       vertex {};
	vk::PipelineInputAssemblyStateCreateInfo                     assembly {};
	vk::PipelineTessellationStateCreateInfo                      tessellation {};
	vk::PipelineViewportStateCreateInfo                          viewport {};
	vk::PipelineViewportDepthClipControlCreateInfoEXT            clip_control {};
	vk::PipelineRasterizationStateCreateInfo                     raster {};
	vk::PipelineRasterizationDepthClipStateCreateInfoEXT         depth_clip {};
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT   provoking {};
	vk::PipelineMultisampleStateCreateInfo                       multisample {};
	vk::PipelineDepthStencilStateCreateInfo                      depth_stencil {};
	vk::PipelineColorBlendStateCreateInfo                        blend {};
	std::vector<vk::PipelineColorBlendAttachmentState>           blend_attachments;
	vk::PipelineColorWriteCreateInfoEXT                          color_write {};
	std::vector<vk::Bool32>                                      color_write_enables;
	vk::PipelineDynamicStateCreateInfo                           dynamic {};
	std::vector<vk::DynamicState>                                dynamic_states;
	vk::PipelineRenderingCreateInfo                              rendering {};
	std::vector<vk::Format>                                      color_formats;
	vk::GraphicsPipelineCreateInfo                               info {};
	Layout                                                       layout;
	uint32_t                                                     flags = 0;
};

template <typename T>
inline bool GetStruct(Reader& r, T& value, bool& present) {
	uint8_t flag = 0;
	if (!r.Get(flag)) {
		return false;
	}
	present = flag != 0;
	if (present) {
		r.Get(value);
		// A damaged record: the struct would reach the driver typed as something else.
		if (value.sType != T::structureType) {
			r.Fail();
		}
		value.pNext = nullptr;
	}
	return true;
}

inline bool ParseGraphics(std::span<const uint8_t> payload, GraphicsReplay& g) {
	Reader   r(payload);
	uint32_t stage_count = 0;
	r.Get(g.flags);
	if (!r.Get(stage_count) || stage_count > 6) {
		return false;
	}
	for (uint32_t i = 0; i < stage_count; i++) {
		uint32_t stage = 0;
		uint64_t hash  = 0;
		r.Get(stage);
		r.Get(hash);
		vk::PipelineShaderStageCreateInfo info {};
		info.stage = static_cast<vk::ShaderStageFlagBits>(stage);
		info.pName = "main";
		g.stages.push_back(info);
		g.module_hashes.push_back(hash);
	}
	uint8_t has_vertex = 0;
	r.Get(has_vertex);
	if (has_vertex != 0) {
		r.GetArray(g.vertex_bindings);
		r.GetArray(g.vertex_attributes);
	}
	bool has_assembly = false, has_tess = false, has_viewport = false, has_clip_control = false,
	     has_raster = false, has_depth_clip = false, has_provoking = false, has_multisample = false,
	     has_depth_stencil = false, has_blend = false;
	GetStruct(r, g.assembly, has_assembly);
	GetStruct(r, g.tessellation, has_tess);
	GetStruct(r, g.viewport, has_viewport);
	GetStruct(r, g.clip_control, has_clip_control);
	GetStruct(r, g.raster, has_raster);
	GetStruct(r, g.depth_clip, has_depth_clip);
	GetStruct(r, g.provoking, has_provoking);
	GetStruct(r, g.multisample, has_multisample);
	GetStruct(r, g.depth_stencil, has_depth_stencil);
	GetStruct(r, g.blend, has_blend);
	uint8_t has_color_write = 0;
	if (has_blend) {
		r.GetArray(g.blend_attachments);
		r.Get(has_color_write);
		if (has_color_write != 0) {
			r.GetArray(g.color_write_enables);
		}
	}
	uint8_t has_dynamic = 0;
	r.Get(has_dynamic);
	if (has_dynamic != 0) {
		r.GetArray(g.dynamic_states);
	}
	r.Get(g.rendering.viewMask);
	r.GetArray(g.color_formats);
	r.Get(g.rendering.depthAttachmentFormat);
	r.Get(g.rendering.stencilAttachmentFormat);
	if (!GetLayout(r, g.layout) || !r.Ok()) {
		return false;
	}
	// Structs are stored whole; replay rebuilds only the pointers set below. Any other stored
	// pointer is an address in the process that recorded it (SerializeGraphics refuses them).
	if ((has_viewport && (g.viewport.pViewports != nullptr || g.viewport.pScissors != nullptr)) ||
	    (has_multisample && g.multisample.pSampleMask != nullptr)) {
		return false;
	}

	g.info.flags      = vk::PipelineCreateFlags(g.flags);
	g.info.stageCount = static_cast<uint32_t>(g.stages.size());
	g.info.pStages    = g.stages.data();
	if (has_vertex != 0) {
		g.vertex.vertexBindingDescriptionCount   = static_cast<uint32_t>(g.vertex_bindings.size());
		g.vertex.pVertexBindingDescriptions      = g.vertex_bindings.data();
		g.vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(g.vertex_attributes.size());
		g.vertex.pVertexAttributeDescriptions    = g.vertex_attributes.data();
		g.info.pVertexInputState                 = &g.vertex;
	}
	g.info.pInputAssemblyState = has_assembly ? &g.assembly : nullptr;
	g.info.pTessellationState  = has_tess ? &g.tessellation : nullptr;
	if (has_viewport) {
		g.viewport.pNext      = has_clip_control ? &g.clip_control : nullptr;
		g.info.pViewportState = &g.viewport;
	}
	if (has_raster) {
		const void* next = nullptr;
		if (has_depth_clip) {
			g.depth_clip.pNext = nullptr;
			next               = &g.depth_clip;
		}
		if (has_provoking) {
			g.provoking.pNext = next;
			next              = &g.provoking;
		}
		g.raster.pNext             = next;
		g.info.pRasterizationState = &g.raster;
	}
	g.info.pMultisampleState  = has_multisample ? &g.multisample : nullptr;
	g.info.pDepthStencilState = has_depth_stencil ? &g.depth_stencil : nullptr;
	if (has_blend) {
		g.blend.attachmentCount = static_cast<uint32_t>(g.blend_attachments.size());
		g.blend.pAttachments    = g.blend_attachments.data();
		if (has_color_write != 0) {
			g.color_write.attachmentCount    = static_cast<uint32_t>(g.color_write_enables.size());
			g.color_write.pColorWriteEnables = g.color_write_enables.data();
			g.blend.pNext                    = &g.color_write;
		}
		g.info.pColorBlendState = &g.blend;
	}
	if (has_dynamic != 0) {
		g.dynamic.dynamicStateCount = static_cast<uint32_t>(g.dynamic_states.size());
		g.dynamic.pDynamicStates    = g.dynamic_states.data();
		g.info.pDynamicState        = &g.dynamic;
	}
	g.rendering.colorAttachmentCount    = static_cast<uint32_t>(g.color_formats.size());
	g.rendering.pColorAttachmentFormats = g.color_formats.data();
	g.info.pNext                        = &g.rendering;
	g.info.basePipelineIndex            = -1;
	return true;
}

struct ComputeReplay {
	uint64_t module_hash    = 0;
	uint32_t subgroup_size  = 0;
	Layout   layout;
};

inline bool ParseCompute(std::span<const uint8_t> payload, ComputeReplay& c) {
	Reader r(payload);
	r.Get(c.module_hash);
	r.Get(c.subgroup_size);
	return GetLayout(r, c.layout) && r.Ok();
}

} // namespace Libs::Graphics::PipelinePrewarm

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_
