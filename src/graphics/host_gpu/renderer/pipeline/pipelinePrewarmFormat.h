#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>
#include <xxhash.h>

// Record checks of the pipeline prewarm file that need no Vulkan device (unit-tested in
// tests/PipelinePrewarmFormatTests.cpp).
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

} // namespace Libs::Graphics::PipelinePrewarm

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEPREWARMFORMAT_H_
