#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HITCHSTATS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HITCHSTATS_H_

#include <cstdint>

// KYTY_HITCH_LOG=1: for every guest frame whose GPU-command-thread time exceeds 45 ms, prints
// where that time went (shader translation, pipeline builds, texture and buffer uploads, waits
// for the GPU, waiting for the guest to submit work). Only the GPU command thread is measured;
// a long frame spent mostly idle means the guest itself was late (its CPU threads or loading).
namespace Libs::Graphics::HitchStats {

enum class Category : uint32_t {
	ShaderCompile,
	PipelineBuild,
	TextureUpload,
	BufferUpload,
	GpuWait,
	GuestIdle,
	FlipQueueWait,
	Count,
};

[[nodiscard]] bool Enabled();
// Marks the calling thread as the GPU command thread (the only one measured).
void BindThisThread();
void Add(Category category, uint64_t qpc_ticks);
void CountTexture(uint64_t bytes);
// GPU command thread, at each guest flip.
void EndGuestFrame();

class Scope {
public:
	explicit Scope(Category category);
	~Scope();
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Category m_category;
	uint64_t m_begin = 0;
};

} // namespace Libs::Graphics::HitchStats

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_HITCHSTATS_H_
