#include "graphics/host_gpu/renderer/pipeline/pipelinePrewarm.h"
#include "graphics/host_gpu/renderer/pipeline/pipelinePrewarmFormat.h"

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
	// Pointers inside stored structs are not rebuilt on replay (ParseGraphics rejects them).
	if ((info.pMultisampleState != nullptr && info.pMultisampleState->pSampleMask != nullptr) ||
	    (info.pViewportState != nullptr && (info.pViewportState->pViewports != nullptr ||
	                                        info.pViewportState->pScissors != nullptr))) {
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


} // namespace

void Open(const std::filesystem::path& path) {
	auto&           state = GetState();
	std::lock_guard lock(state.mutex);
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	bool     valid            = false;
	uint32_t rejected_modules = 0;
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
				uint64_t              hash = 0;
				std::vector<uint32_t> words;
				if (!DecodeModuleRecord(record.payload, hash, words)) {
					// Dropped and not rewritten below. Pipelines using it count as failed in this
					// replay; RecordModule stores the module again when the title next creates it.
					rejected_modules++;
					continue;
				}
				state.modules.emplace(hash, std::move(words));
			} else if (record.kind == Kind::Graphics || record.kind == Kind::Compute) {
				if (state.pipelines.insert(XXH3_64bits(record.payload.data(), size)).second) {
					state.loaded.push_back(std::move(record));
				}
			}
		}
	}
	if (rejected_modules != 0) {
		std::printf("Pipeline prewarm: dropped %u damaged shader module record(s) from %s\n",
		            rejected_modules, path.string().c_str());
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
