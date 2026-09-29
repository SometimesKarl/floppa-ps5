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

} // namespace

int main() {
	TestValidRecordRoundTrips();
	TestEveryFlippedByteIsRejected();
	TestShapeChecks();
	std::printf("PipelinePrewarmFormatTests: %d checks passed\n", g_checks);
	return 0;
}
