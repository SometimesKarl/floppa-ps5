#include "graphics/host_gpu/renderer/pipeline/pipelinePrewarmFormat.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Libs::Graphics::PipelinePrewarm::DecodeModuleRecord;
using Libs::Graphics::PipelinePrewarm::SpirvMagic;

int g_checks = 0;

void Check(bool value, const char* message) {
	g_checks++;
	if (!value) {
		std::fprintf(stderr, "PipelinePrewarmFormatTests: failed: %s\n", message);
		std::abort();
	}
}

// Same layout as PipelinePrewarm::RecordModule writes: u64 XXH3 of the words, then the words.
std::vector<uint8_t> MakeRecord(const std::vector<uint32_t>& words) {
	const uint64_t       hash = XXH3_64bits(words.data(), words.size() * sizeof(uint32_t));
	std::vector<uint8_t> payload(sizeof(hash) + words.size() * sizeof(uint32_t));
	std::memcpy(payload.data(), &hash, sizeof(hash));
	std::memcpy(payload.data() + sizeof(hash), words.data(), words.size() * sizeof(uint32_t));
	return payload;
}

std::vector<uint32_t> SampleModule() {
	// SPIR-V header (magic, version 1.3, generator, bound, schema) and a few instruction words.
	return {SpirvMagic, 0x00010300u, 0u, 16u, 0u, 0x00020011u, 1u, 0x0003000eu, 0u, 1u};
}

void TestValidRecordRoundTrips() {
	const auto            words   = SampleModule();
	const auto            payload = MakeRecord(words);
	uint64_t              hash    = 0;
	std::vector<uint32_t> decoded;
	Check(DecodeModuleRecord(payload, hash, decoded), "valid record rejected");
	Check(decoded == words, "decoded words differ");
	Check(hash == XXH3_64bits(words.data(), words.size() * sizeof(uint32_t)), "hash differs");
}

void TestEveryFlippedByteIsRejected() {
	const auto base = MakeRecord(SampleModule());
	for (size_t byte = 0; byte < base.size(); byte++) {
		for (const uint8_t mask: {uint8_t {0x01}, uint8_t {0x80}}) {
			auto payload = base;
			payload[byte] ^= mask;
			uint64_t              hash = 0;
			std::vector<uint32_t> words;
			Check(!DecodeModuleRecord(payload, hash, words), "damaged record accepted");
		}
	}
}

void TestShapeChecks() {
	uint64_t              hash = 0;
	std::vector<uint32_t> words;
	const auto            base = MakeRecord(SampleModule());

	Check(!DecodeModuleRecord({}, hash, words), "empty payload accepted");
	Check(!DecodeModuleRecord(std::span(base).first(sizeof(uint64_t)), hash, words),
	      "hash-only payload accepted");
	// Shorter than a SPIR-V header, even with a matching hash.
	const auto short_record = MakeRecord({SpirvMagic, 0x00010300u, 0u, 1u});
	Check(!DecodeModuleRecord(short_record, hash, words), "record shorter than a header accepted");
	// A payload that is not whole words.
	auto ragged = base;
	ragged.push_back(0);
	Check(!DecodeModuleRecord(ragged, hash, words), "ragged payload accepted");
	// Consistent hash but not SPIR-V.
	auto not_spirv = SampleModule();
	not_spirv[0]   = 0x12345678u;
	Check(!DecodeModuleRecord(MakeRecord(not_spirv), hash, words), "non-SPIR-V record accepted");
	// A torn record (bytes lost at the end) keeps its hash but loses words.
	const auto torn = std::span(base).first(base.size() - sizeof(uint32_t));
	Check(!DecodeModuleRecord(torn, hash, words), "torn record accepted");
}

using Libs::Graphics::PipelinePrewarm::ComputeReplay;
using Libs::Graphics::PipelinePrewarm::GraphicsReplay;
using Libs::Graphics::PipelinePrewarm::ParseCompute;
using Libs::Graphics::PipelinePrewarm::ParseGraphics;
using Libs::Graphics::PipelinePrewarm::Writer;

// Same encoding as PutStruct in pipelinePrewarm.cpp: a presence byte, then the whole struct.
template <typename T>
void PutPresent(Writer& w, T value) {
	w.Put(static_cast<uint8_t>(1));
	value.pNext = nullptr;
	w.Put(value);
}

void PutAbsent(Writer& w) {
	w.Put(static_cast<uint8_t>(0));
}

void PutLayout(Writer& w) {
	w.Put(uint32_t {0});           // set flags
	w.Put(uint32_t {1});           // bindings
	w.Put(uint32_t {0});           // binding
	w.Put(static_cast<uint32_t>(vk::DescriptorType::eStorageBuffer));
	w.Put(uint32_t {1});           // descriptor count
	w.Put(static_cast<uint32_t>(vk::ShaderStageFlagBits::eFragment));
	w.Put(static_cast<uint32_t>(vk::ShaderStageFlagBits::eVertex)); // push stages
	w.Put(uint32_t {0});           // push offset
	w.Put(uint32_t {16});          // push size
}

struct GraphicsFields {
	vk::PipelineViewportStateCreateInfo      viewport {};
	vk::PipelineRasterizationStateCreateInfo raster {};
	vk::PipelineMultisampleStateCreateInfo   multisample {};
	uint32_t                                 dynamic_count = 2;
};

// A record in the field order ParseGraphics reads.
std::vector<uint8_t> GraphicsRecord(const GraphicsFields& fields) {
	Writer w;
	w.Put(uint32_t {0}); // flags
	w.Put(uint32_t {2}); // stages
	w.Put(static_cast<uint32_t>(vk::ShaderStageFlagBits::eVertex));
	w.Put(uint64_t {0x1111});
	w.Put(static_cast<uint32_t>(vk::ShaderStageFlagBits::eFragment));
	w.Put(uint64_t {0x2222});
	w.Put(static_cast<uint8_t>(1)); // vertex input
	w.Put(uint32_t {0});            // bindings
	w.Put(uint32_t {0});            // attributes
	vk::PipelineInputAssemblyStateCreateInfo assembly {};
	assembly.topology = vk::PrimitiveTopology::eTriangleList;
	PutPresent(w, assembly);
	PutAbsent(w); // tessellation
	PutPresent(w, fields.viewport);
	PutAbsent(w); // depth clip control
	PutPresent(w, fields.raster);
	PutAbsent(w); // depth clip
	PutAbsent(w); // provoking vertex
	PutPresent(w, fields.multisample);
	PutPresent(w, vk::PipelineDepthStencilStateCreateInfo {});
	PutPresent(w, vk::PipelineColorBlendStateCreateInfo {});
	w.Put(uint32_t {1}); // blend attachments
	w.Put(vk::PipelineColorBlendAttachmentState {});
	w.Put(static_cast<uint8_t>(0)); // no color write enables
	w.Put(static_cast<uint8_t>(1)); // dynamic states
	w.Put(fields.dynamic_count);
	for (uint32_t i = 0; i < fields.dynamic_count; i++) {
		w.Put(i % 2 == 0 ? vk::DynamicState::eViewportWithCount
		                 : vk::DynamicState::eScissorWithCount);
	}
	w.Put(uint32_t {0}); // view mask
	w.Put(uint32_t {1}); // color formats
	w.Put(vk::Format::eR8G8B8A8Unorm);
	w.Put(vk::Format::eD32Sfloat);
	w.Put(vk::Format::eUndefined);
	PutLayout(w);
	return w.data;
}

void TestGraphicsRecordRoundTrips() {
	GraphicsReplay g;
	Check(ParseGraphics(GraphicsRecord({}), g), "valid graphics record rejected");
	Check(g.stages.size() == 2 && g.module_hashes[1] == 0x2222 &&
	          g.info.stageCount == 2 && g.info.pStages == g.stages.data(),
	      "stages not restored");
	Check(g.info.pColorBlendState == &g.blend && g.blend.attachmentCount == 1 &&
	          g.blend.pAttachments == g.blend_attachments.data(),
	      "blend attachments not rewired");
	Check(g.info.pDynamicState == &g.dynamic && g.dynamic.dynamicStateCount == 2,
	      "dynamic states not rewired");
	Check(g.info.pNext == &g.rendering && g.rendering.colorAttachmentCount == 1 &&
	          g.color_formats[0] == vk::Format::eR8G8B8A8Unorm &&
	          g.rendering.depthAttachmentFormat == vk::Format::eD32Sfloat,
	      "rendering formats not restored");
	Check(g.layout.bindings.size() == 1 && g.layout.push.size == 16, "layout not restored");
	Check(g.info.pTessellationState == nullptr && g.info.pViewportState == &g.viewport,
	      "optional structs wired wrongly");
}

void TestGraphicsRecordRejections() {
	// A stored pointer is an address in the process that recorded it.
	const auto stale = [](auto mutate, const char* message) {
		GraphicsFields fields;
		mutate(fields);
		GraphicsReplay g;
		Check(!ParseGraphics(GraphicsRecord(fields), g), message);
	};
	stale([](GraphicsFields& f) { f.viewport.pViewports = reinterpret_cast<const vk::Viewport*>(0x1000); },
	      "stale viewport pointer accepted");
	stale([](GraphicsFields& f) { f.viewport.pScissors = reinterpret_cast<const vk::Rect2D*>(0x1000); },
	      "stale scissor pointer accepted");
	stale([](GraphicsFields& f) { f.multisample.pSampleMask = reinterpret_cast<const vk::SampleMask*>(0x1000); },
	      "stale sample mask pointer accepted");
	stale([](GraphicsFields& f) { f.raster.sType = vk::StructureType::ePipelineDepthStencilStateCreateInfo; },
	      "struct with the wrong sType accepted");
	stale([](GraphicsFields& f) { f.dynamic_count = 5000; }, "oversized array count accepted");

	const auto valid = GraphicsRecord({});
	for (size_t size = 0; size < valid.size(); size++) {
		GraphicsReplay g;
		Check(!ParseGraphics(std::span(valid).first(size), g), "truncated graphics record accepted");
	}
	auto trailing = valid;
	trailing.push_back(0);
	GraphicsReplay g;
	Check(!ParseGraphics(trailing, g), "graphics record with trailing bytes accepted");
}

void TestComputeRecord() {
	Writer w;
	w.Put(uint64_t {0x3333}); // module hash
	w.Put(uint32_t {64});     // required subgroup size
	PutLayout(w);
	ComputeReplay c;
	Check(ParseCompute(w.data, c) && c.module_hash == 0x3333 && c.subgroup_size == 64 &&
	          c.layout.bindings.size() == 1,
	      "valid compute record rejected or not restored");
	for (size_t size = 0; size < w.data.size(); size++) {
		ComputeReplay truncated;
		Check(!ParseCompute(std::span(w.data).first(size), truncated),
		      "truncated compute record accepted");
	}
}

} // namespace

int main() {
	TestValidRecordRoundTrips();
	TestEveryFlippedByteIsRejected();
	TestShapeChecks();
	TestGraphicsRecordRoundTrips();
	TestGraphicsRecordRejections();
	TestComputeRecord();
	std::printf("PipelinePrewarmFormatTests: %d checks passed\n", g_checks);
	return 0;
}
