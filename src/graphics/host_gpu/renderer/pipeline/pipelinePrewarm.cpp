#include "graphics/host_gpu/renderer/pipeline/pipelinePrewarm.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xxhash.h>

namespace Libs::Graphics::PipelinePrewarm {

namespace {

// File: "KYPW" + version, then records { u8 kind, u32 size, payload }. Module payloads are
// { u64 hash, SPIR-V words }; pipeline payloads are their serialized create info (below), whose
// XXH3 is their identity.
constexpr uint32_t Magic   = 0x5750594bu; // "KYPW"
constexpr uint32_t Version = 1;

enum class Kind : uint8_t { Module = 1, Graphics = 2, Compute = 3 };

struct Record {
	Kind                 kind = Kind::Module;
	std::vector<uint8_t> payload;
};

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

private:
	std::span<const uint8_t> m_bytes;
	size_t                   m_offset = 0;
	bool                     m_ok     = true;
};

struct State {
	std::mutex                                                   mutex;
	std::ofstream                                                file;
	std::unordered_map<uint64_t, std::vector<uint32_t>>          modules;  // hash -> SPIR-V
	std::unordered_map<VkShaderModule, uint64_t>                 handles;  // live module -> hash
	std::unordered_set<uint64_t>                                 pipelines; // payload hashes
	std::vector<Record>                                          loaded;   // earlier sessions
};

State& GetState() {
	static State state;
	return state;
}

void Append(State& state, Kind kind, const std::vector<uint8_t>& payload) {
	if (!state.file.is_open()) {
		return;
	}
	const auto kind_byte = static_cast<uint8_t>(kind);
	const auto size      = static_cast<uint32_t>(payload.size());
	state.file.write(reinterpret_cast<const char*>(&kind_byte), 1);
	state.file.write(reinterpret_cast<const char*>(&size), sizeof(size));
	state.file.write(reinterpret_cast<const char*>(payload.data()),
	                 static_cast<std::streamsize>(payload.size()));
	state.file.flush();
}

// The descriptor-set layout and push-constant range every recorded pipeline layout consists of.
void PutLayout(Writer& w, std::span<const vk::DescriptorSetLayoutBinding> bindings,
               vk::DescriptorSetLayoutCreateFlags set_flags, const vk::PushConstantRange& push) {
	w.Put(static_cast<uint32_t>(set_flags));
	w.Put(static_cast<uint32_t>(bindings.size()));
	for (const auto& binding: bindings) {
		EXIT_IF(binding.pImmutableSamplers != nullptr);
		w.Put(binding.binding);
		w.Put(static_cast<uint32_t>(binding.descriptorType));
		w.Put(binding.descriptorCount);
		w.Put(static_cast<uint32_t>(binding.stageFlags));
	}
	w.Put(static_cast<uint32_t>(push.stageFlags));
	w.Put(push.offset);
	w.Put(push.size);
}

struct Layout {
	uint32_t                                     set_flags = 0;
	std::vector<vk::DescriptorSetLayoutBinding>  bindings;
	vk::PushConstantRange                        push {};
};

bool GetLayout(Reader& r, Layout& layout) {
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

// Writes the fixed-function structs of a graphics pipeline. Pointers and pNext are not stored:
// the chains this emulator builds are rebuilt from flags on replay (see BuildGraphics).
template <typename T>
void PutStruct(Writer& w, const T* value) {
	w.Put(static_cast<uint8_t>(value != nullptr));
	if (value == nullptr) {
		return;
	}
	auto copy  = *value;
	copy.pNext = nullptr;
	w.Put(copy);
}

template <typename T>
const T* FindNext(const void* next, vk::StructureType type) {
	for (const auto* base = static_cast<const vk::BaseInStructure*>(next); base != nullptr;
	     base             = base->pNext) {
		if (base->sType == type) {
			return reinterpret_cast<const T*>(base);
		}
	}
	return nullptr;
}

bool SerializeGraphics(const State& state, const vk::GraphicsPipelineCreateInfo& info,
                       std::span<const vk::DescriptorSetLayoutBinding> bindings,
                       vk::DescriptorSetLayoutCreateFlags set_flags, const vk::PushConstantRange& push,
                       Writer& w) {
	constexpr auto StoredFlags = vk::PipelineCreateFlagBits::eDisableOptimization;
	w.Put(static_cast<uint32_t>(info.flags & StoredFlags));
	w.Put(info.stageCount);
	for (uint32_t i = 0; i < info.stageCount; i++) {
		const auto& stage = info.pStages[i];
		const auto  found = state.handles.find(static_cast<VkShaderModule>(stage.module));
		if (found == state.handles.end() || stage.pSpecializationInfo != nullptr ||
		    stage.pNext != nullptr) {
			return false;
		}
		w.Put(static_cast<uint32_t>(stage.stage));
		w.Put(found->second);
	}
	const auto* vertex = info.pVertexInputState;
	w.Put(static_cast<uint8_t>(vertex != nullptr));
	if (vertex != nullptr) {
		w.PutArray(vertex->pVertexBindingDescriptions, vertex->vertexBindingDescriptionCount);
		w.PutArray(vertex->pVertexAttributeDescriptions, vertex->vertexAttributeDescriptionCount);
	}
	PutStruct(w, info.pInputAssemblyState);
	PutStruct(w, info.pTessellationState);
	PutStruct(w, info.pViewportState);
	const auto* clip_control =
	    info.pViewportState == nullptr
	        ? nullptr
	        : FindNext<vk::PipelineViewportDepthClipControlCreateInfoEXT>(
	              info.pViewportState->pNext,
	              vk::StructureType::ePipelineViewportDepthClipControlCreateInfoEXT);
	PutStruct(w, clip_control);
	PutStruct(w, info.pRasterizationState);
	const void* raster_next = info.pRasterizationState != nullptr ? info.pRasterizationState->pNext
	                                                               : nullptr;
	PutStruct(w, FindNext<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>(
	                 raster_next, vk::StructureType::ePipelineRasterizationDepthClipStateCreateInfoEXT));
	PutStruct(w, FindNext<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>(
	                 raster_next,
	                 vk::StructureType::ePipelineRasterizationProvokingVertexStateCreateInfoEXT));
	if (info.pMultisampleState != nullptr && info.pMultisampleState->pSampleMask != nullptr) {
		return false;
	}
	PutStruct(w, info.pMultisampleState);
	PutStruct(w, info.pDepthStencilState);
	const auto* blend = info.pColorBlendState;
	PutStruct(w, blend);
	if (blend != nullptr) {
		w.PutArray(blend->pAttachments, blend->attachmentCount);
		const auto* write = FindNext<vk::PipelineColorWriteCreateInfoEXT>(
		    blend->pNext, vk::StructureType::ePipelineColorWriteCreateInfoEXT);
		w.Put(static_cast<uint8_t>(write != nullptr));
		if (write != nullptr) {
			w.PutArray(write->pColorWriteEnables, write->attachmentCount);
		}
	}
	const auto* dynamic = info.pDynamicState;
	w.Put(static_cast<uint8_t>(dynamic != nullptr));
	if (dynamic != nullptr) {
		w.PutArray(dynamic->pDynamicStates, dynamic->dynamicStateCount);
	}
	const auto* rendering = FindNext<vk::PipelineRenderingCreateInfo>(
	    info.pNext, vk::StructureType::ePipelineRenderingCreateInfo);
	if (rendering == nullptr) {
		return false;
	}
	w.Put(rendering->viewMask);
	w.PutArray(rendering->pColorAttachmentFormats, rendering->colorAttachmentCount);
	w.Put(rendering->depthAttachmentFormat);
	w.Put(rendering->stencilAttachmentFormat);
	PutLayout(w, bindings, set_flags, push);
	return true;
}

void Store(Kind kind, Writer& w) {
	auto&      state = GetState();
	const auto hash  = XXH3_64bits(w.data.data(), w.data.size());
	if (!state.pipelines.insert(hash).second) {
		return;
	}
	Append(state, kind, w.data);
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
bool GetStruct(Reader& r, T& value, bool& present) {
	uint8_t flag = 0;
	if (!r.Get(flag)) {
		return false;
	}
	present = flag != 0;
	if (present) {
		r.Get(value);
		value.pNext = nullptr;
	}
	return true;
}

bool ParseGraphics(std::span<const uint8_t> payload, GraphicsReplay& g) {
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

bool ParseCompute(std::span<const uint8_t> payload, ComputeReplay& c) {
	Reader r(payload);
	r.Get(c.module_hash);
	r.Get(c.subgroup_size);
	return GetLayout(r, c.layout) && r.Ok();
}

} // namespace

void Open(const std::filesystem::path& path) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	bool valid = false;
	if (std::ifstream in(path, std::ios::binary); in) {
		uint32_t magic = 0, version = 0;
		in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
		in.read(reinterpret_cast<char*>(&version), sizeof(version));
		valid = in && magic == Magic && version == Version;
		while (valid) {
			uint8_t  kind = 0;
			uint32_t size = 0;
			if (!in.read(reinterpret_cast<char*>(&kind), 1) ||
			    !in.read(reinterpret_cast<char*>(&size), sizeof(size)) || size > (64u << 20u)) {
				break;
			}
			Record record {static_cast<Kind>(kind), std::vector<uint8_t>(size)};
			if (!in.read(reinterpret_cast<char*>(record.payload.data()), size)) {
				break; // a torn tail from an interrupted session: keep what came before
			}
			if (record.kind == Kind::Module) {
				if (size < sizeof(uint64_t) || (size - sizeof(uint64_t)) % 4 != 0) {
					continue;
				}
				uint64_t hash = 0;
				std::memcpy(&hash, record.payload.data(), sizeof(hash));
				std::vector<uint32_t> words((size - sizeof(uint64_t)) / 4);
				std::memcpy(words.data(), record.payload.data() + sizeof(hash), words.size() * 4);
				state.modules.emplace(hash, std::move(words));
			} else if (record.kind == Kind::Graphics || record.kind == Kind::Compute) {
				if (state.pipelines.insert(XXH3_64bits(record.payload.data(), size)).second) {
					state.loaded.push_back(std::move(record));
				}
			}
		}
	}
	// A file from another format is replaced; a valid one is appended to (its torn tail, if any,
	// is rewritten cleanly below).
	std::vector<Record> modules;
	if (valid) {
		state.file.open(path, std::ios::binary | std::ios::trunc);
		const uint32_t header[2] {Magic, Version};
		state.file.write(reinterpret_cast<const char*>(header), sizeof(header));
		for (const auto& [hash, words]: state.modules) {
			Writer w;
			w.Put(hash);
			for (const auto word: words) {
				w.Put(word);
			}
			Append(state, Kind::Module, w.data);
		}
		for (const auto& record: state.loaded) {
			Append(state, record.kind, record.payload);
		}
	} else {
		state.modules.clear();
		state.loaded.clear();
		state.pipelines.clear();
		state.file.open(path, std::ios::binary | std::ios::trunc);
		const uint32_t header[2] {Magic, Version};
		state.file.write(reinterpret_cast<const char*>(header), sizeof(header));
		state.file.flush();
	}
}

void Close() {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	if (state.file.is_open()) {
		state.file.close();
	}
}

void RecordModule(vk::ShaderModule module, std::span<const uint32_t> spirv) {
	auto&           state = GetState();
	const auto      hash  = XXH3_64bits(spirv.data(), spirv.size_bytes());
	std::lock_guard lock(state.mutex);
	state.handles[static_cast<VkShaderModule>(module)] = hash;
	if (state.modules.contains(hash)) {
		return;
	}
	state.modules.emplace(hash, std::vector<uint32_t>(spirv.begin(), spirv.end()));
	Writer w;
	w.Put(hash);
	for (const auto word: spirv) {
		w.Put(word);
	}
	Append(state, Kind::Module, w.data);
}

void ForgetModule(vk::ShaderModule module) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	state.handles.erase(static_cast<VkShaderModule>(module));
}

void RecordGraphics(const vk::GraphicsPipelineCreateInfo&            info,
                    std::span<const vk::DescriptorSetLayoutBinding> bindings,
                    vk::DescriptorSetLayoutCreateFlags set_flags, const vk::PushConstantRange& push) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	if (!state.file.is_open()) {
		return;
	}
	Writer w;
	if (SerializeGraphics(state, info, bindings, set_flags, push, w)) {
		Store(Kind::Graphics, w);
	}
}

void RecordCompute(const vk::PipelineShaderStageCreateInfo&         stage,
                   std::span<const vk::DescriptorSetLayoutBinding> bindings,
                   vk::DescriptorSetLayoutCreateFlags set_flags, const vk::PushConstantRange& push) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	if (!state.file.is_open()) {
		return;
	}
	const auto found = state.handles.find(static_cast<VkShaderModule>(stage.module));
	if (found == state.handles.end() || stage.pSpecializationInfo != nullptr) {
		return;
	}
	const auto* subgroup = FindNext<vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo>(
	    stage.pNext, vk::StructureType::ePipelineShaderStageRequiredSubgroupSizeCreateInfo);
	Writer w;
	w.Put(found->second);
	w.Put(subgroup != nullptr ? subgroup->requiredSubgroupSize : 0u);
	PutLayout(w, bindings, set_flags, push);
	Store(Kind::Compute, w);
}

ReplayStats Replay(GraphicContext& graphics, vk::PipelineCache driver_cache, uint32_t threads,
                   const std::function<void(uint32_t, uint32_t)>& progress) {
	auto&               state = GetState();
	std::vector<Record> records;
	{
		std::lock_guard lock(state.mutex);
		records = state.loaded;
	}
	ReplayStats stats;
	stats.total = static_cast<uint32_t>(records.size());
	if (records.empty()) {
		return stats;
	}
	const auto begin = std::chrono::steady_clock::now();
	auto       device = graphics.device;

	// Modules are shared by many pipelines: create each once, up front.
	std::unordered_map<uint64_t, vk::ShaderModule> modules;
	const auto module_for = [&](uint64_t hash) -> vk::ShaderModule {
		if (const auto found = modules.find(hash); found != modules.end()) {
			return found->second;
		}
		vk::ShaderModule module = nullptr;
		{
			std::lock_guard lock(state.mutex);
			const auto      found = state.modules.find(hash);
			if (found != state.modules.end()) {
				vk::ShaderModuleCreateInfo create {};
				create.codeSize = found->second.size() * sizeof(uint32_t);
				create.pCode    = found->second.data();
				if (device.createShaderModule(&create, nullptr, &module) != vk::Result::eSuccess) {
					module = nullptr;
				}
			}
		}
		modules.emplace(hash, module);
		return module;
	};
	for (const auto& record: records) {
		if (record.kind == Kind::Graphics) {
			GraphicsReplay g;
			if (ParseGraphics(record.payload, g)) {
				for (const auto hash: g.module_hashes) {
					module_for(hash);
				}
			}
		} else {
			ComputeReplay c;
			if (ParseCompute(record.payload, c)) {
				module_for(c.module_hash);
			}
		}
	}

	std::atomic<uint32_t> next {0}, done {0}, cached {0}, compiled {0}, failed {0};
	const bool            probe = graphics.pipeline_cache_control_enabled;
	const auto create_layout = [&](const Layout& layout, vk::DescriptorSetLayout& set,
	                               vk::PipelineLayout& pipeline_layout) {
		vk::DescriptorSetLayoutCreateInfo set_info {};
		set_info.flags        = vk::DescriptorSetLayoutCreateFlags(layout.set_flags);
		set_info.bindingCount = static_cast<uint32_t>(layout.bindings.size());
		set_info.pBindings    = layout.bindings.data();
		if (device.createDescriptorSetLayout(&set_info, nullptr, &set) != vk::Result::eSuccess) {
			return false;
		}
		vk::PipelineLayoutCreateInfo info {};
		info.setLayoutCount         = 1;
		info.pSetLayouts            = &set;
		info.pushConstantRangeCount = layout.push.size != 0 ? 1u : 0u;
		info.pPushConstantRanges    = &layout.push;
		return device.createPipelineLayout(&info, nullptr, &pipeline_layout) == vk::Result::eSuccess;
	};
	// Creates the pipeline from the driver cache when it is there; compiles it otherwise.
	const auto create = [&](auto&& create_one) {
		vk::Pipeline pipeline = nullptr;
		if (probe && create_one(vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired,
		                        pipeline) == vk::Result::eSuccess &&
		    pipeline != nullptr) {
			cached++;
		} else if (create_one(vk::PipelineCreateFlags {}, pipeline) == vk::Result::eSuccess &&
		           pipeline != nullptr) {
			compiled++;
		} else {
			failed++;
		}
		if (pipeline != nullptr) {
			device.destroyPipeline(pipeline, nullptr);
		}
	};
	// Normal priority: the guest's first draw waits for this.
	const auto work = [&] {
		for (;;) {
			const auto index = next.fetch_add(1);
			if (index >= records.size()) {
				return;
			}
			const auto&             record = records[index];
			vk::DescriptorSetLayout set    = nullptr;
			vk::PipelineLayout      layout = nullptr;
			if (record.kind == Kind::Graphics) {
				GraphicsReplay g;
				bool ok = ParseGraphics(record.payload, g);
				for (size_t i = 0; ok && i < g.stages.size(); i++) {
					g.stages[i].module = modules.at(g.module_hashes[i]);
					ok                 = g.stages[i].module != nullptr;
				}
				if (ok && create_layout(g.layout, set, layout)) {
					g.info.layout = layout;
					create([&](vk::PipelineCreateFlags extra, vk::Pipeline& out) {
						auto info = g.info;
						info.flags |= extra;
						return device.createGraphicsPipelines(driver_cache, 1, &info, nullptr, &out);
					});
				} else {
					failed++;
				}
			} else {
				ComputeReplay c;
				const bool    ok = ParseCompute(record.payload, c) && modules.at(c.module_hash) != nullptr;
				if (ok && create_layout(c.layout, set, layout)) {
					vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup {};
					vk::ComputePipelineCreateInfo                         info {};
					info.stage.stage  = vk::ShaderStageFlagBits::eCompute;
					info.stage.module = modules.at(c.module_hash);
					info.stage.pName  = "main";
					if (c.subgroup_size != 0) {
						subgroup.requiredSubgroupSize = c.subgroup_size;
						info.stage.pNext              = &subgroup;
					}
					info.layout            = layout;
					info.basePipelineIndex = -1;
					create([&](vk::PipelineCreateFlags extra, vk::Pipeline& out) {
						auto create_info = info;
						create_info.flags |= extra;
						return device.createComputePipelines(driver_cache, 1, &create_info, nullptr,
						                                     &out);
					});
				} else {
					failed++;
				}
			}
			if (layout != nullptr) {
				device.destroyPipelineLayout(layout, nullptr);
			}
			if (set != nullptr) {
				device.destroyDescriptorSetLayout(set, nullptr);
			}
			done++;
		}
	};
	std::vector<std::thread> workers;
	for (uint32_t i = 0; i < std::max(threads, 1u); i++) {
		workers.emplace_back(work);
	}
	while (done.load() < records.size()) {
		progress(done.load(), static_cast<uint32_t>(records.size()));
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	for (auto& worker: workers) {
		worker.join();
	}
	for (const auto& [hash, module]: modules) {
		if (module != nullptr) {
			device.destroyShaderModule(module, nullptr);
		}
	}
	stats.cached   = cached.load();
	stats.compiled = compiled.load();
	stats.failed   = failed.load();
	stats.seconds  = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	return stats;
}

} // namespace Libs::Graphics::PipelinePrewarm
