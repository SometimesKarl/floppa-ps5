// pipebench: measures how long the host driver takes to compile the pipelines a KytyPS5 session
// recorded (_PipelineCache/<title>.pipelines, PipelinePrewarm format), outside the emulator.
//
//   pipebench FILE.pipelines [--top N] [--mode full|opt|gpl|all] [--list]
//
// Every module gets a unique OpSourceExtension per run so the driver's own on-disk shader cache
// cannot answer, and no VkPipelineCache is used: each time is a cold compile.
//   full: one vkCreateGraphicsPipelines per record (what the emulator does today).
//   opt:  the same after spirv-opt's performance passes (optimizer time reported separately).
//   gpl:  VK_EXT_graphics_pipeline_library: vertex-input, pre-rasterization (vertex stages),
//         fragment-shader and fragment-output libraries (the two shader libraries compiled on two
//         threads), then a fast link and, for comparison, a link-time-optimized link.

#define VK_NO_PROTOTYPES
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_EXCEPTIONS
#include <vulkan/vulkan.hpp>

#include <spirv-tools/libspirv.hpp>
#include <spirv-tools/optimizer.hpp>

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
	return std::chrono::duration<double, std::milli>(b - a).count();
}

// ---- PipelinePrewarm record format (copied from pipelinePrewarm.cpp) ----------------------------

constexpr uint32_t Magic   = 0x5750594bu;
constexpr uint32_t Version = 1;
enum class Kind : uint8_t { Module = 1, Graphics = 2, Compute = 3 };

class Reader {
public:
	explicit Reader(std::span<const uint8_t> bytes): m_bytes(bytes) {}
	template <typename T>
	bool Get(T& value) {
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

struct Layout {
	uint32_t                                    set_flags = 0;
	std::vector<vk::DescriptorSetLayoutBinding> bindings;
	vk::PushConstantRange                       push {};
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

struct Graphics {
	std::vector<uint64_t>                                      module_hashes;
	std::vector<vk::PipelineShaderStageCreateInfo>             stages;
	std::vector<vk::VertexInputBindingDescription>             vertex_bindings;
	std::vector<vk::VertexInputAttributeDescription>           vertex_attributes;
	vk::PipelineVertexInputStateCreateInfo                     vertex {};
	vk::PipelineInputAssemblyStateCreateInfo                   assembly {};
	vk::PipelineTessellationStateCreateInfo                    tessellation {};
	vk::PipelineViewportStateCreateInfo                        viewport {};
	vk::PipelineViewportDepthClipControlCreateInfoEXT          clip_control {};
	vk::PipelineRasterizationStateCreateInfo                   raster {};
	vk::PipelineRasterizationDepthClipStateCreateInfoEXT       depth_clip {};
	vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking {};
	vk::PipelineMultisampleStateCreateInfo                     multisample {};
	vk::PipelineDepthStencilStateCreateInfo                    depth_stencil {};
	vk::PipelineColorBlendStateCreateInfo                      blend {};
	std::vector<vk::PipelineColorBlendAttachmentState>         blend_attachments;
	vk::PipelineColorWriteCreateInfoEXT                        color_write {};
	std::vector<vk::Bool32>                                    color_write_enables;
	vk::PipelineDynamicStateCreateInfo                         dynamic {};
	std::vector<vk::DynamicState>                              dynamic_states;
	vk::PipelineRenderingCreateInfo                            rendering {};
	std::vector<vk::Format>                                    color_formats;
	vk::GraphicsPipelineCreateInfo                             info {};
	Layout                                                     layout;
	uint32_t                                                   flags = 0;
	bool has_vertex = false, has_assembly = false, has_tess = false, has_viewport = false,
	     has_raster = false, has_multisample = false, has_depth_stencil = false, has_blend = false,
	     has_dynamic = false;
};

bool ParseGraphics(std::span<const uint8_t> payload, Graphics& g) {
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
	g.has_vertex = has_vertex != 0;
	if (g.has_vertex) {
		r.GetArray(g.vertex_bindings);
		r.GetArray(g.vertex_attributes);
	}
	bool has_clip_control = false, has_depth_clip = false, has_provoking = false;
	GetStruct(r, g.assembly, g.has_assembly);
	GetStruct(r, g.tessellation, g.has_tess);
	GetStruct(r, g.viewport, g.has_viewport);
	GetStruct(r, g.clip_control, has_clip_control);
	GetStruct(r, g.raster, g.has_raster);
	GetStruct(r, g.depth_clip, has_depth_clip);
	GetStruct(r, g.provoking, has_provoking);
	GetStruct(r, g.multisample, g.has_multisample);
	GetStruct(r, g.depth_stencil, g.has_depth_stencil);
	GetStruct(r, g.blend, g.has_blend);
	uint8_t has_color_write = 0;
	if (g.has_blend) {
		r.GetArray(g.blend_attachments);
		r.Get(has_color_write);
		if (has_color_write != 0) {
			r.GetArray(g.color_write_enables);
		}
	}
	uint8_t has_dynamic = 0;
	r.Get(has_dynamic);
	g.has_dynamic = has_dynamic != 0;
	if (g.has_dynamic) {
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
	if (g.has_vertex) {
		g.vertex.vertexBindingDescriptionCount   = static_cast<uint32_t>(g.vertex_bindings.size());
		g.vertex.pVertexBindingDescriptions      = g.vertex_bindings.data();
		g.vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(g.vertex_attributes.size());
		g.vertex.pVertexAttributeDescriptions    = g.vertex_attributes.data();
		g.info.pVertexInputState                 = &g.vertex;
	}
	g.info.pInputAssemblyState = g.has_assembly ? &g.assembly : nullptr;
	g.info.pTessellationState  = g.has_tess ? &g.tessellation : nullptr;
	if (g.has_viewport) {
		g.viewport.pNext      = has_clip_control ? &g.clip_control : nullptr;
		g.info.pViewportState = &g.viewport;
	}
	if (g.has_raster) {
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
	g.info.pMultisampleState  = g.has_multisample ? &g.multisample : nullptr;
	g.info.pDepthStencilState = g.has_depth_stencil ? &g.depth_stencil : nullptr;
	if (g.has_blend) {
		g.blend.attachmentCount = static_cast<uint32_t>(g.blend_attachments.size());
		g.blend.pAttachments    = g.blend_attachments.data();
		if (has_color_write != 0) {
			g.color_write.attachmentCount    = static_cast<uint32_t>(g.color_write_enables.size());
			g.color_write.pColorWriteEnables = g.color_write_enables.data();
			g.blend.pNext                    = &g.color_write;
		}
		g.info.pColorBlendState = &g.blend;
	}
	if (g.has_dynamic) {
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

// ---- SPIR-V helpers ------------------------------------------------------------------------------

// Inserts OpSourceExtension "<tag>" at the start of the debug section, so the module hashes
// differently (a driver-cache miss) without changing its code.
std::vector<uint32_t> Tagged(const std::vector<uint32_t>& spirv, const std::string& tag) {
	size_t pos = 5;
	while (pos < spirv.size()) {
		const uint32_t op    = spirv[pos] & 0xffffu;
		const uint32_t words = spirv[pos] >> 16u;
		// Capability, Extension, ExtInstImport, MemoryModel, EntryPoint, ExecutionMode(Id).
		if (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16 && op != 331) {
			break;
		}
		pos += std::max(words, 1u);
	}
	std::vector<uint32_t> literal((tag.size() + 4) / 4, 0);
	std::memcpy(literal.data(), tag.data(), tag.size());
	std::vector<uint32_t> out(spirv.begin(), spirv.begin() + static_cast<ptrdiff_t>(pos));
	out.push_back((static_cast<uint32_t>(literal.size() + 1) << 16u) | 4u); // OpSourceExtension
	out.insert(out.end(), literal.begin(), literal.end());
	out.insert(out.end(), spirv.begin() + static_cast<ptrdiff_t>(pos), spirv.end());
	return out;
}

bool Optimize(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
	optimizer.SetMessageConsumer([](spv_message_level_t, const char*, const spv_position_t&,
	                                const char*) {});
	optimizer.RegisterPerformancePasses();
	spvtools::ValidatorOptions validator;
	validator.SetScalarBlockLayout(true);
	spvtools::OptimizerOptions options;
	options.set_run_validator(false);
	options.set_validator_options(validator);
	return optimizer.Run(in.data(), in.size(), &out, options);
}

// ---- Vulkan setup --------------------------------------------------------------------------------

struct Device {
	vk::Instance       instance;
	vk::PhysicalDevice physical;
	vk::Device         device;
	bool               gpl = false;
};

bool CreateDevice(Device& d) {
	HMODULE library = LoadLibraryA("vulkan-1.dll");
	if (library == nullptr) {
		std::printf("vulkan-1.dll not found\n");
		return false;
	}
	auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
	    GetProcAddress(library, "vkGetInstanceProcAddr"));
	VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc);
	vk::ApplicationInfo app {};
	app.pApplicationName = "pipebench";
	app.apiVersion       = VK_API_VERSION_1_3;
	vk::InstanceCreateInfo instance_info {};
	instance_info.pApplicationInfo = &app;
	if (vk::createInstance(&instance_info, nullptr, &d.instance) != vk::Result::eSuccess) {
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(d.instance);
	uint32_t count = 0;
	(void)d.instance.enumeratePhysicalDevices(&count, nullptr);
	std::vector<vk::PhysicalDevice> devices(count);
	(void)d.instance.enumeratePhysicalDevices(&count, devices.data());
	for (auto candidate: devices) {
		if (candidate.getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu) {
			d.physical = candidate;
		}
	}
	if (!d.physical && !devices.empty()) {
		d.physical = devices.front();
	}
	if (!d.physical) {
		return false;
	}
	const auto props = d.physical.getProperties();
	std::printf("device: %s, driver %u.%u.%u\n", props.deviceName.data(),
	            VK_API_VERSION_MAJOR(props.driverVersion), VK_API_VERSION_MINOR(props.driverVersion),
	            VK_API_VERSION_PATCH(props.driverVersion));

	uint32_t ext_count = 0;
	(void)d.physical.enumerateDeviceExtensionProperties(nullptr, &ext_count, nullptr);
	std::vector<vk::ExtensionProperties> available(ext_count);
	(void)d.physical.enumerateDeviceExtensionProperties(nullptr, &ext_count, available.data());
	const auto has = [&](const char* name) {
		return std::ranges::any_of(available, [&](const auto& e) {
			return std::strcmp(e.extensionName.data(), name) == 0;
		});
	};
	// The emulator's device extensions that change pipeline creation, plus pipeline libraries.
	std::vector<const char*> extensions;
	for (const char* name:
	     {VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME, VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME,
	      VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
	      VK_EXT_COLOR_WRITE_ENABLE_EXTENSION_NAME, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
	      VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
	      VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
	      VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
	      VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME}) {
		if (has(name)) {
			extensions.push_back(name);
		}
	}
	d.gpl = has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);

	// Everything the device supports is enabled (robustness included, as in the emulator).
	vk::PhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl {};
	vk::PhysicalDeviceMeshShaderFeaturesEXT              mesh {};
	vk::PhysicalDeviceProvokingVertexFeaturesEXT         provoking {};
	vk::PhysicalDeviceRobustness2FeaturesEXT             robustness2 {};
	vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR barycentric {};
	vk::PhysicalDeviceDepthClipEnableFeaturesEXT         depth_clip {};
	vk::PhysicalDeviceDepthClipControlFeaturesEXT        clip_control {};
	vk::PhysicalDeviceColorWriteEnableFeaturesEXT        color_write {};
	vk::PhysicalDeviceVulkan13Features                   f13 {};
	vk::PhysicalDeviceVulkan12Features                   f12 {};
	vk::PhysicalDeviceVulkan11Features                   f11 {};
	vk::PhysicalDeviceFeatures2                          f2 {};
	f2.pNext           = &f11;
	f11.pNext          = &f12;
	f12.pNext          = &f13;
	f13.pNext          = &color_write;
	color_write.pNext  = &clip_control;
	clip_control.pNext = &depth_clip;
	depth_clip.pNext   = &barycentric;
	barycentric.pNext  = &robustness2;
	robustness2.pNext  = &provoking;
	provoking.pNext    = has(VK_EXT_MESH_SHADER_EXTENSION_NAME) ? static_cast<void*>(&mesh) : nullptr;
	mesh.pNext         = d.gpl ? &gpl : nullptr;
	if (!has(VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
		provoking.pNext = d.gpl ? &gpl : nullptr;
	}
	d.physical.getFeatures2(&f2);
	mesh.multiviewMeshShader                    = VK_FALSE;
	mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
	provoking.transformFeedbackPreservesProvokingVertex = VK_FALSE;

	const float                 priority = 1.0f;
	vk::DeviceQueueCreateInfo   queue {};
	queue.queueFamilyIndex = 0;
	queue.queueCount       = 1;
	queue.pQueuePriorities = &priority;
	vk::DeviceCreateInfo create {};
	create.pNext                   = &f2;
	create.queueCreateInfoCount    = 1;
	create.pQueueCreateInfos       = &queue;
	create.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
	create.ppEnabledExtensionNames = extensions.data();
	const auto result              = d.physical.createDevice(&create, nullptr, &d.device);
	if (result != vk::Result::eSuccess) {
		std::printf("vkCreateDevice: %s\n", vk::to_string(result).c_str());
		return false;
	}
	VULKAN_HPP_DEFAULT_DISPATCHER.init(d.device);
	if (d.gpl) {
		vk::PhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl_props {};
		vk::PhysicalDeviceProperties2                          props2 {};
		props2.pNext = &gpl_props;
		d.physical.getProperties2(&props2);
		std::printf("graphics pipeline library: fast linking %s, independent interpolation %s\n",
		            gpl_props.graphicsPipelineLibraryFastLinking ? "yes" : "no",
		            gpl_props.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes"
		                                                                                 : "no");
	}
	return true;
}

// ---- Benchmark -------------------------------------------------------------------------------------

struct Record {
	std::vector<uint8_t> payload;
	uint64_t             words = 0;
	size_t               index = 0;
};

struct Layouts {
	vk::DescriptorSetLayout set {};
	vk::PipelineLayout      pipeline {};
};

bool CreateLayout(vk::Device device, const Layout& layout, Layouts& out) {
	vk::DescriptorSetLayoutCreateInfo set_info {};
	set_info.flags        = vk::DescriptorSetLayoutCreateFlags(layout.set_flags);
	set_info.bindingCount = static_cast<uint32_t>(layout.bindings.size());
	set_info.pBindings    = layout.bindings.data();
	if (device.createDescriptorSetLayout(&set_info, nullptr, &out.set) != vk::Result::eSuccess) {
		return false;
	}
	vk::PipelineLayoutCreateInfo info {};
	info.setLayoutCount         = 1;
	info.pSetLayouts            = &out.set;
	info.pushConstantRangeCount = layout.push.size != 0 ? 1u : 0u;
	info.pPushConstantRanges    = &layout.push;
	return device.createPipelineLayout(&info, nullptr, &out.pipeline) == vk::Result::eSuccess;
}

void DestroyLayout(vk::Device device, Layouts& l) {
	if (l.pipeline) device.destroyPipelineLayout(l.pipeline, nullptr);
	if (l.set) device.destroyDescriptorSetLayout(l.set, nullptr);
}

vk::ShaderModule CreateModule(vk::Device device, const std::vector<uint32_t>& spirv) {
	vk::ShaderModuleCreateInfo info {};
	info.codeSize = spirv.size() * 4;
	info.pCode    = spirv.data();
	vk::ShaderModule module {};
	if (device.createShaderModule(&info, nullptr, &module) != vk::Result::eSuccess) {
		return {};
	}
	return module;
}

struct Result {
	double ms = -1;
	bool   ok = false;
};

Result CreateFull(vk::Device device, vk::GraphicsPipelineCreateInfo info) {
	vk::Pipeline pipeline {};
	const auto   begin  = Clock::now();
	const auto   result = device.createGraphicsPipelines({}, 1, &info, nullptr, &pipeline);
	const auto   end    = Clock::now();
	if (pipeline) device.destroyPipeline(pipeline, nullptr);
	return {Ms(begin, end), result == vk::Result::eSuccess};
}

struct GplTimes {
	double vertex_input = 0, pre_raster = 0, fragment = 0, output = 0, parallel_wall = 0,
	       fast_link = 0, lto_link = 0;
	bool   ok = false;
};

// retain: libraries keep link-time-optimization info, and the link is optimized (the
// background-optimized pipeline); otherwise libraries are final and the link is a fast link.
GplTimes CreateGpl(vk::Device device, const Graphics& g, vk::PipelineLayout layout,
                   std::span<const vk::PipelineShaderStageCreateInfo> stages, bool retain) {
	GplTimes t;
	using F   = vk::GraphicsPipelineLibraryFlagBitsEXT;
	const auto library_flags =
	    retain ? vk::PipelineCreateFlagBits::eLibraryKHR |
	                 vk::PipelineCreateFlagBits::eRetainLinkTimeOptimizationInfoEXT
	           : vk::PipelineCreateFlags {vk::PipelineCreateFlagBits::eLibraryKHR};
	std::vector<vk::PipelineShaderStageCreateInfo> vertex_stages, fragment_stages;
	for (const auto& s: stages) {
		(s.stage == vk::ShaderStageFlagBits::eFragment ? fragment_stages : vertex_stages).push_back(s);
	}
	auto base = g.info;
	base.flags |= library_flags;
	auto rendering = g.rendering;
	const auto make = [&](F part, vk::GraphicsPipelineLibraryCreateInfoEXT& lib,
	                      vk::GraphicsPipelineCreateInfo& info) {
		lib       = vk::GraphicsPipelineLibraryCreateInfoEXT {};
		lib.flags = part;
		info      = base;
		info.pNext = &lib;
		lib.pNext  = &rendering;
		info.stageCount = 0;
		info.pStages    = nullptr;
		info.pVertexInputState = nullptr;
		info.pInputAssemblyState = nullptr;
		info.pTessellationState = nullptr;
		info.pViewportState = nullptr;
		info.pRasterizationState = nullptr;
		info.pMultisampleState = nullptr;
		info.pDepthStencilState = nullptr;
		info.pColorBlendState = nullptr;
		info.layout = nullptr;
		switch (part) {
			case F::eVertexInputInterface:
				info.pVertexInputState   = g.info.pVertexInputState;
				info.pInputAssemblyState = g.info.pInputAssemblyState;
				break;
			case F::ePreRasterizationShaders:
				info.stageCount          = static_cast<uint32_t>(vertex_stages.size());
				info.pStages             = vertex_stages.data();
				info.pTessellationState  = g.info.pTessellationState;
				info.pViewportState      = g.info.pViewportState;
				info.pRasterizationState = g.info.pRasterizationState;
				info.layout              = layout;
				break;
			case F::eFragmentShader:
				info.stageCount         = static_cast<uint32_t>(fragment_stages.size());
				info.pStages            = fragment_stages.data();
				info.pMultisampleState  = g.info.pMultisampleState;
				info.pDepthStencilState = g.info.pDepthStencilState;
				info.layout             = layout;
				break;
			case F::eFragmentOutputInterface:
				info.pMultisampleState = g.info.pMultisampleState;
				info.pColorBlendState  = g.info.pColorBlendState;
				break;
			default: break;
		}
	};
	vk::GraphicsPipelineLibraryCreateInfoEXT lib[4];
	vk::GraphicsPipelineCreateInfo           info[4];
	vk::Pipeline                             parts[4] {};
	const F kinds[4] {F::eVertexInputInterface, F::ePreRasterizationShaders, F::eFragmentShader,
	                  F::eFragmentOutputInterface};
	for (int i = 0; i < 4; i++) {
		make(kinds[i], lib[i], info[i]);
	}
	bool ok = true;
	const auto create = [&](int i, double& ms) {
		const auto begin = Clock::now();
		ok &= device.createGraphicsPipelines({}, 1, &info[i], nullptr, &parts[i]) ==
		      vk::Result::eSuccess;
		ms = Ms(begin, Clock::now());
	};
	create(0, t.vertex_input);
	create(3, t.output);
	// The two shader libraries on two threads, as a compile manager would.
	const auto wall_begin = Clock::now();
	std::thread fragment([&] { create(2, t.fragment); });
	create(1, t.pre_raster);
	fragment.join();
	t.parallel_wall = Ms(wall_begin, Clock::now());
	if (ok) {
		vk::PipelineLibraryCreateInfoKHR link_libs {};
		link_libs.libraryCount = 4;
		link_libs.pLibraries   = parts;
		vk::GraphicsPipelineCreateInfo link {};
		link.pNext  = &link_libs;
		link.layout = layout;
		for (int lto = retain ? 1 : 0; lto < (retain ? 2 : 1); lto++) {
			link.flags = lto ? vk::PipelineCreateFlagBits::eLinkTimeOptimizationEXT
			                 : vk::PipelineCreateFlags {};
			vk::Pipeline linked {};
			const auto   begin = Clock::now();
			ok &= device.createGraphicsPipelines({}, 1, &link, nullptr, &linked) ==
			      vk::Result::eSuccess;
			(lto ? t.lto_link : t.fast_link) = Ms(begin, Clock::now());
			if (linked) device.destroyPipeline(linked, nullptr);
		}
	}
	for (auto part: parts) {
		if (part) device.destroyPipeline(part, nullptr);
	}
	t.ok = ok;
	return t;
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::printf("usage: pipebench FILE.pipelines [--top N] [--mode full|opt|gpl|all] [--list]\n");
		return 1;
	}
	size_t      top  = 20;
	size_t      from = 0;
	std::string mode = "all";
	bool        list = false;
	for (int i = 2; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg == "--top" && i + 1 < argc) top = std::stoul(argv[++i]);
		else if (arg == "--from" && i + 1 < argc) from = std::stoul(argv[++i]);
		else if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
		else if (arg == "--list") list = true;
	}
	std::ifstream in(argv[1], std::ios::binary);
	std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
	if (data.size() < 8) {
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}
	uint32_t magic = 0, version = 0;
	std::memcpy(&magic, data.data(), 4);
	std::memcpy(&version, data.data() + 4, 4);
	if (magic != Magic || version != Version) {
		std::printf("not a KYPW v1 file\n");
		return 1;
	}
	std::unordered_map<uint64_t, std::vector<uint32_t>> modules;
	std::vector<Record>                                  graphics;
	for (size_t pos = 8; pos + 5 <= data.size();) {
		const auto kind = static_cast<Kind>(data[pos]);
		uint32_t   size = 0;
		std::memcpy(&size, data.data() + pos + 1, 4);
		pos += 5;
		if (pos + size > data.size()) break;
		std::span<const uint8_t> payload(data.data() + pos, size);
		pos += size;
		if (kind == Kind::Module && size >= 8) {
			uint64_t hash = 0;
			std::memcpy(&hash, payload.data(), 8);
			std::vector<uint32_t> words((size - 8) / 4);
			std::memcpy(words.data(), payload.data() + 8, words.size() * 4);
			modules.emplace(hash, std::move(words));
		} else if (kind == Kind::Graphics) {
			graphics.push_back({std::vector<uint8_t>(payload.begin(), payload.end()), 0,
			                    graphics.size()});
		}
	}
	for (auto& r: graphics) {
		Graphics g;
		if (ParseGraphics(r.payload, g)) {
			for (auto h: g.module_hashes) {
				if (auto it = modules.find(h); it != modules.end()) r.words += it->second.size();
			}
		}
	}
	std::ranges::sort(graphics, [](const Record& a, const Record& b) { return a.words > b.words; });
	std::printf("%zu modules, %zu graphics pipelines\n", modules.size(), graphics.size());
	// --asm N S FILE: pipeline #N compiled with its stage S as is and with that stage replaced by
	// the assembled FILE (validated first), 3 times each, cold.
	for (int i = 2; i + 3 < argc; i++) {
		if (std::string(argv[i]) != "--asm") continue;
		const auto n = std::stoul(argv[i + 1]);
		const auto s = std::stoul(argv[i + 2]);
		Graphics   g;
		if (n >= graphics.size() || !ParseGraphics(graphics[n].payload, g) ||
		    s >= g.module_hashes.size()) {
			return 1;
		}
		std::ifstream        text_in(argv[i + 3]);
		const std::string    text((std::istreambuf_iterator<char>(text_in)), {});
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
		std::string          messages;
		tools.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& p,
		                             const char* m) {
			messages += std::to_string(p.index) + ": " + m + "\n";
		});
		std::vector<uint32_t> replaced;
		// SPIR-V 1.3, the version the recompiler emits (a Vulkan 1.3 environment would stamp 1.6).
		spvtools::SpirvTools assembler(SPV_ENV_VULKAN_1_1);
		assembler.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& p,
		                                 const char* m) {
			messages += std::to_string(p.index) + ": " + m + "\n";
		});
		if (!assembler.Assemble(text, &replaced)) {
			std::printf("assembly failed:\n%s", messages.c_str());
			return 1;
		}
		spvtools::ValidatorOptions validator;
		validator.SetScalarBlockLayout(true);
		if (!tools.Validate(replaced.data(), replaced.size(), validator)) {
			std::printf("validation failed:\n%s", messages.substr(0, 2000).c_str());
			return 1;
		}
		Device d;
		if (!CreateDevice(d)) return 1;
		Layouts layouts;
		if (!CreateLayout(d.device, g.layout, layouts)) return 1;
		g.info.layout            = layouts.pipeline;
		const std::string nonce  = std::to_string(GetTickCount64());
		for (int round = 0; round < 3; round++) {
			for (int variant = 0; variant < 2; variant++) {
				std::vector<vk::ShaderModule> owned;
				auto                          stages = g.stages;
				for (size_t k = 0; k < stages.size(); k++) {
					const auto& words = (variant == 1 && k == s) ? replaced : modules[g.module_hashes[k]];
					owned.push_back(CreateModule(
					    d.device, Tagged(words, "pb-asm-" + nonce + std::to_string(round * 2 + variant))));
					stages[k].module = owned.back();
				}
				auto info    = g.info;
				info.pStages = stages.data();
				const auto r = CreateFull(d.device, info);
				std::printf("round %d %s: %.1f ms%s (stage words %zu)\n", round,
				            variant ? "modified" : "original", r.ms, r.ok ? "" : " FAILED",
				            variant ? replaced.size() : modules[g.module_hashes[s]].size());
				for (auto m: owned) d.device.destroyShaderModule(m, nullptr);
			}
		}
		DestroyLayout(d.device, layouts);
		return 0;
	}
	// --dis N: SPIR-V text of pipeline #N's stages next to the input file.
	for (int i = 2; i + 1 < argc; i++) {
		if (std::string(argv[i]) != "--dis") continue;
		const auto n = std::stoul(argv[i + 1]);
		Graphics   g;
		if (n >= graphics.size() || !ParseGraphics(graphics[n].payload, g)) return 1;
		spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
		for (size_t s = 0; s < g.module_hashes.size(); s++) {
			std::string text;
			const auto& words = modules[g.module_hashes[s]];
			tools.Disassemble(words, &text,
			                  SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES |
			                      SPV_BINARY_TO_TEXT_OPTION_INDENT);
			const auto path = std::string(argv[1]) + ".p" + std::to_string(n) + ".s" +
			                  std::to_string(s) + ".spvasm";
			std::ofstream(path) << text;
			std::printf("wrote %s (%zu words)\n", path.c_str(), words.size());
		}
		return 0;
	}
	if (list) {
		for (size_t i = 0; i < graphics.size() && i < top; i++) {
			std::printf("#%zu record %zu: %llu SPIR-V words\n", i, graphics[i].index,
			            static_cast<unsigned long long>(graphics[i].words));
		}
		return 0;
	}
	Device d;
	if (!CreateDevice(d)) return 1;
	const std::string nonce = std::to_string(GetTickCount64());
	const bool do_full = mode == "full" || mode == "all";
	const bool do_opt  = mode == "opt" || mode == "all";
	const bool do_gpl  = (mode == "gpl" || mode == "all") && d.gpl;
	double sum_full = 0, sum_opt = 0, sum_opt_cpu = 0, sum_gpl_wall = 0, sum_gpl_serial = 0,
	       sum_fast = 0, sum_lto = 0;
	uint64_t sum_words = 0, sum_opt_words = 0;
	int      n = 0;
	std::printf("%-4s %8s | %9s | %9s %9s %8s | %8s %8s %8s %8s | %8s %8s\n", "#", "words",
	            "full ms", "spvopt ms", "opt ms", "opt wds", "gpl vs", "gpl ps", "gpl wall",
	            "fastlink", "rtn wall", "ltolink");
	for (size_t i = from; i < graphics.size() && i < from + top; i++) {
		Graphics g;
		if (!ParseGraphics(graphics[i].payload, g)) continue;
		Layouts layouts;
		if (!CreateLayout(d.device, g.layout, layouts)) {
			std::printf("#%zu: layout failed\n", i);
			continue;
		}
		g.info.layout = layouts.pipeline;
		std::vector<std::vector<uint32_t>> sources;
		bool missing = false;
		for (auto h: g.module_hashes) {
			auto it = modules.find(h);
			if (it == modules.end()) missing = true;
			else sources.push_back(it->second);
		}
		if (missing) {
			DestroyLayout(d.device, layouts);
			continue;
		}
		const auto build_stages = [&](const std::vector<std::vector<uint32_t>>& spirv,
		                              const std::string& tag, std::vector<vk::ShaderModule>& owned) {
			std::vector<vk::PipelineShaderStageCreateInfo> stages = g.stages;
			for (size_t s = 0; s < stages.size(); s++) {
				owned.push_back(CreateModule(d.device, Tagged(spirv[s], tag)));
				stages[s].module = owned.back();
			}
			return stages;
		};
		const auto release = [&](std::vector<vk::ShaderModule>& owned) {
			for (auto m: owned) if (m) d.device.destroyShaderModule(m, nullptr);
			owned.clear();
		};
		Result   full {}, opt {};
		double   opt_cpu = 0;
		uint64_t opt_words = 0;
		GplTimes gt {}, gr {};
		std::vector<vk::ShaderModule> owned;
		if (do_full) {
			auto stages       = build_stages(sources, "pb-full-" + nonce + "-" + std::to_string(i), owned);
			auto info         = g.info;
			info.pStages      = stages.data();
			full              = CreateFull(d.device, info);
			release(owned);
		}
		if (do_opt) {
			std::vector<std::vector<uint32_t>> optimized(sources.size());
			const auto begin = Clock::now();
			bool ok = true;
			for (size_t s = 0; s < sources.size(); s++) {
				ok &= Optimize(sources[s], optimized[s]);
				opt_words += optimized[s].size();
			}
			opt_cpu = Ms(begin, Clock::now());
			if (ok) {
				auto stages  = build_stages(optimized, "pb-opt-" + nonce + "-" + std::to_string(i), owned);
				auto info    = g.info;
				info.pStages = stages.data();
				opt          = CreateFull(d.device, info);
				release(owned);
			}
		}
		if (do_gpl) {
			auto stages = build_stages(sources, "pb-gpl-" + nonce + "-" + std::to_string(i), owned);
			gt          = CreateGpl(d.device, g, layouts.pipeline, stages, false);
			release(owned);
			stages = build_stages(sources, "pb-gplr-" + nonce + "-" + std::to_string(i), owned);
			gr     = CreateGpl(d.device, g, layouts.pipeline, stages, true);
			gt.lto_link = gr.lto_link;
			release(owned);
		}
		DestroyLayout(d.device, layouts);
		std::printf("%-4zu %8llu | %9.1f | %9.1f %9.1f %8llu | %8.1f %8.1f %8.1f %8.2f | %8.1f %8.1f%s\n",
		            i, static_cast<unsigned long long>(graphics[i].words), full.ok ? full.ms : -1.0,
		            opt_cpu, opt.ok ? opt.ms : -1.0, static_cast<unsigned long long>(opt_words),
		            gt.pre_raster, gt.fragment, gt.parallel_wall, gt.fast_link, gr.parallel_wall,
		            gt.lto_link, do_gpl && !(gt.ok && gr.ok) ? "  (gpl failed)" : "");
		std::fflush(stdout);
		n++;
		sum_words += graphics[i].words;
		sum_full += full.ok ? full.ms : 0;
		sum_opt += opt.ok ? opt.ms : 0;
		sum_opt_cpu += opt_cpu;
		sum_opt_words += opt_words;
		sum_gpl_wall += gt.parallel_wall;
		sum_gpl_serial += gt.pre_raster + gt.fragment;
		sum_fast += gt.fast_link;
		sum_lto += gt.lto_link;
	}
	std::printf("total over %d pipelines (%llu words): full %.0f ms | spirv-opt %.0f ms + compile %.0f "
	            "ms (%llu words) | gpl libraries %.0f ms serial, %.0f ms on 2 threads, fast link "
	            "%.1f ms, LTO link %.0f ms\n",
	            n, static_cast<unsigned long long>(sum_words), sum_full, sum_opt_cpu, sum_opt,
	            static_cast<unsigned long long>(sum_opt_words), sum_gpl_serial, sum_gpl_wall,
	            sum_fast, sum_lto);
	d.device.destroy();
	d.instance.destroy();
	return 0;
}
